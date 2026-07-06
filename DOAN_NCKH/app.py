import cv2
import easyocr
import re
import time
import serial
import sqlite3
import threading
from datetime import datetime
from flask import Flask, render_template, Response, jsonify, request

import serial.tools.list_ports

# ================== SERIAL WRITE LOCKS ==================
# Camera loop (thread) + Flask request threads đều có thể ghi Serial.
# Khóa giúp tránh bị "dính" lệnh vào nhau.
serial_lock_in = threading.Lock()
serial_lock_out = threading.Lock()

def _sanitize_field(s: str) -> str:
    """Tránh phá protocol (| và xuống dòng) khi gửi xuống ESP32."""
    return (s or "").replace("|", " ").replace("\n", " ").replace("\r", " ").strip()

def safe_write(ser, lock: threading.Lock, line: str) -> bool:
    """Ghi 1 dòng xuống Serial một cách an toàn. line có/không có \n đều được."""
    if ser is None:
        return False
    if not line.endswith("\n"):
        line += "\n"
    try:
        with lock:
            ser.write(line.encode())
        return True
    except Exception as e:
        print("[SERIAL][ERROR] write failed:", e)
        return False

def send_to_both(line: str) -> None:
    """Broadcast 1 lệnh xuống cả ESP32 IN và OUT."""
    ok_in = safe_write(arduino_in, serial_lock_in, line)
    ok_out = safe_write(arduino_out, serial_lock_out, line)
    print(f"[SERIAL][BROADCAST] {line.strip()} | IN={ok_in} OUT={ok_out}")

def sync_all_rfid_cards_to_esp32():
    """
    Gửi toàn bộ thẻ RFID trong DB xuống cả 2 ESP32.
    Cơ chế: RESET_CARDS -> gửi ADD|... từng thẻ
    """
    try:
        print("[SYNC_ALL] Start sync all RFID cards to ESP32...")

        # 1) Reset danh sách thẻ trên 2 ESP32
        send_to_both("RESET_CARDS")

        # 2) Lấy tất cả thẻ từ DB
        conn = sqlite3.connect(DB_FILE)
        cur = conn.cursor()
        cur.execute("""
            SELECT uid, COALESCE(owner_name,''), COALESCE(plate,''), COALESCE(user_type,'guest'), COALESCE(status,'active')
            FROM rfid_cards
            ORDER BY id ASC
        """)
        rows = cur.fetchall()
        conn.close()

        # 3) Gửi từng thẻ xuống ESP32 bằng đúng format ADD đang dùng
        sent = 0
        for uid, owner, plate, user_type, status in rows:
            uid = (uid or "").strip()
            if not uid:
                continue

            cmd = f"ADD|{uid}|{owner}|{plate}|{user_type}|{status}"
            send_to_both(cmd)
            sent += 1
            time.sleep(0.05)  # ✅ nhịp nhỏ tránh ngộp Serial

        print(f"[SYNC_ALL] Done. Sent {sent} cards to both ESP32.")

    except Exception as e:
        print("[SYNC_ALL][ERROR]", e)


def safe_serial(port):
    try:
        ser = serial.Serial(port, 9600, timeout=1)
        print(f"[OK] Connected to {port}")
        return ser
    except serial.SerialException as e:
        print(f"[ERROR] {port} busy or unavailable: {e}")
        return None

arduino_in = safe_serial("COM5")
arduino_out = safe_serial("COM12")


# ==== CẤU HÌNH CAMERA ====
cap_in = cv2.VideoCapture(0)
cap_out = cv2.VideoCapture(1)

reader = easyocr.Reader(['en', 'vi'])
pattern = re.compile(r"^\d{2}[A-Z]-?\d{4,5}$")
required_count = 3
timeout = 2

# ==== BIẾN TOÀN CỤC ====
last_plate_in, last_plate_out = "", ""
plate_counter_in, plate_counter_out = {}, {}
last_seen_time_in, last_seen_time_out = time.time(), time.time()
# ===== RFID SCAN STATE =====
scan_enabled = False
scan_start_time = 0
scan_timeout = 15  # giây

last_scanned_uid = None
scan_result = None  # "success" | "duplicate" | "timeout"


# ==== SQLITE ====
DB_FILE = "parking.db"
def init_db():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            time TEXT,
            uid TEXT,
            name TEXT,
            plate TEXT,
            direction TEXT
        )
    """)
    conn.commit()
    conn.close()

def upgrade_db():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    # Bảng lưu lượt gửi xe (1 dòng = 1 lượt gửi)
    c.execute("""
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid TEXT,
            name TEXT,
            plate TEXT,
            start_time TEXT,
            end_time TEXT,
            duration REAL,
            fee REAL,
            status TEXT DEFAULT 'IN'   -- IN hoặc OUT
        )
    """)

    # Bảng quản lý thẻ RFID
    c.execute("""
        CREATE TABLE IF NOT EXISTS rfid_cards (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid TEXT UNIQUE,
            owner_name TEXT,
            plate TEXT,
            user_type TEXT,   -- resident / guest
            status TEXT DEFAULT 'active'  -- active / inactive
        )
    """)

    conn.commit()
    conn.close()


def save_log(uid, name, plate, direction):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("INSERT INTO logs (time, uid, name, plate, direction) VALUES (?, ?, ?, ?, ?)",
              (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), uid, name, plate, direction))
    conn.commit()
    conn.close()
    print(f"[DB] Saved: {uid}, {name}, {plate}, {direction}")

def calc_fee(duration_min):
    """Tính phí gửi xe (5.000đ/giờ đầu, 3.000đ/giờ tiếp theo)"""
    duration_hr = duration_min / 60
    if duration_hr <= 1:
        return 5000
    else:
        return 5000 + (duration_hr - 1) * 3000

def handle_session(uid, name, plate, direction):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    if direction == "In":
        # Tạo lượt gửi mới
        c.execute("""
            INSERT INTO sessions (uid, name, plate, start_time, status)
            VALUES (?, ?, ?, ?, 'IN')
        """, (uid, name, plate, now))
        print(f"[SESSION] Xe {plate} vào lúc {now}")

    elif direction == "Out":
        # Tìm lượt đang mở
        c.execute("""
            SELECT id, start_time FROM sessions
            WHERE uid=? AND plate=? AND status='IN'
            ORDER BY id DESC LIMIT 1
        """, (uid, plate))
        row = c.fetchone()
        if row:
            session_id, start_time = row
            start_dt = datetime.strptime(start_time, "%Y-%m-%d %H:%M:%S")
            end_dt = datetime.now()
            duration = (end_dt - start_dt).total_seconds() / 60  # phút
            fee = calc_fee(duration)

            c.execute("""
                UPDATE sessions
                SET end_time=?, duration=?, fee=?, status='OUT'
                WHERE id=?
            """, (end_dt.strftime("%Y-%m-%d %H:%M:%S"), duration, fee, session_id))

            print(f"[SESSION] Xe {plate} ra lúc {end_dt}, {duration:.1f} phút, phí {fee:.0f}đ")
        else:
            print(f"[WARN] Không tìm thấy lượt gửi đang mở cho UID {uid} - biển {plate}")

    conn.commit()
    conn.close()

# ==== OCR & XỬ LÝ FRAME ====
def process_frame(frame, plate_counter, last_plate, last_seen_time, prefix, ser, lock=None):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    results = reader.readtext(gray)
    parts = []

    for (bbox, text, prob) in results:
        text = re.sub(r'[^A-Z0-9]', '', text.upper())
        if len(text) >= 2 and prob > 0.5:
            parts.append(text)
            (top_left, _, bottom_right, _) = bbox
            top_left = tuple(map(int, top_left))
            bottom_right = tuple(map(int, bottom_right))
            cv2.rectangle(frame, top_left, bottom_right, (0, 255, 0), 2)
            cv2.putText(frame, text, (top_left[0], top_left[1] - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)

    candidate = None
    if len(parts) == 1:
        candidate = parts[0]
    elif len(parts) >= 2:
        candidate = parts[0] + "" + "".join(parts[1:])

    if candidate and pattern.match(candidate):
        plate_counter[candidate] = plate_counter.get(candidate, 0) + 1
        if plate_counter[candidate] >= required_count and candidate != last_plate:
            print(f"{prefix} Biển số:", candidate)
            last_plate = candidate
            last_seen_time = time.time()
            plate_counter.clear()

            # gửi xuống ESP32 (nếu serial đang kết nối)
            line = prefix + candidate
            if ser is not None:
                if lock is None:
                    # fallback: không khóa
                    ser.write((line + "\n").encode())
                else:
                    safe_write(ser, lock, line)
                print("Sent to Arduino:", line)

    if time.time() - last_seen_time > timeout:
        last_plate = ""

    return frame, plate_counter, last_plate, last_seen_time


# ==== ĐỌC TỪ ARDUINO ====
def read_from_arduino(ser, ser_out=None):
    global current_slots
    global scan_enabled, last_scanned_uid, scan_result
    
    if ser.in_waiting > 0:
        line = ser.readline().decode(errors="ignore").strip()

        if line.startswith("UID:"):
            print("[SCAN][UID RAW]", line)


        # ===== RFID UID SCAN (THÊM Ở ĐÂY) =====
        global scan_enabled, scan_result, last_scanned_uid, scan_start_time

        if scan_enabled and line.startswith("UID:"):
            uid = line.replace("UID:", "").strip()
            if not uid:
                return

            try:
                conn = sqlite3.connect(DB_FILE)
                cur = conn.cursor()

                cur.execute("SELECT id FROM rfid_cards WHERE uid = ?", (uid,))
                exists = cur.fetchone()

                if exists:
                    scan_result = "duplicate"
                else:
                    # mặc định: guest + active, chỉ uid
                    cur.execute("""
                        INSERT INTO rfid_cards (uid, owner_name, plate, user_type, status)
                        VALUES (?, ?, ?, ?, ?)
                    """, (uid, "Guest", "", "guest", "active"))
                    conn.commit()

                    # GỬI XUỐNG 2 ESP32 – GIỮ NGUYÊN CƠ CHẾ CŨ
                    send_to_both(f"ADD|{uid}|Guest||guest|active")

                    scan_result = "success"

                last_scanned_uid = uid
                scan_enabled = False

            except Exception as e:
                print("[SCAN][ERROR]", e)
                scan_result = "error"
                last_scanned_uid = uid
                scan_enabled = False

            finally:
                try:
                    conn.close()
                except:
                    pass

            # tắt scanMode ở ESP32 để quay lại chạy bình thường
            send_to_both("SCAN_OFF")

            return  # ⬅️ QUAN TRỌNG: KHÔNG CHẠY XUỐNG DATA
        # ======================================  

        if line.startswith("DATA"):
            print("[LOG]", line)
            try:
                parts = line.split(",")
                uid = parts[3] if len(parts) > 3 else ""
                name = parts[4] if len(parts) > 4 else ""
                plate = parts[5] if len(parts) > 5 else ""
                direction = parts[6] if len(parts) > 6 else ""
                save_log(uid, name, plate, direction)
                handle_session(uid, name, plate, direction)

                if name == "Guest" and direction == "In" and ser_out is not None:
                    reg_cmd = f"REG:{uid},{plate}"
                    safe_write(ser_out, serial_lock_out, reg_cmd)
                    print("[SYNC] Sent to Arduino OUT:", reg_cmd)

                if name == "Guest" and direction == "Out" and arduino_in is not None:
                    del_cmd = f"DEL:{uid}"
                    safe_write(arduino_in, serial_lock_in, del_cmd)
                    print("[SYNC] Sent DEL to Arduino IN:", del_cmd)

            except Exception as e:
                print("[ERROR] Parse log failed:", e)


# ==== HÀM MỞ / ĐÓNG BARRIER ====
def open_gate(gate='in'):
    ser = arduino_in if gate == 'in' else arduino_out
    if gate == 'in':
        safe_write(ser, serial_lock_in, 'OPEN_GATE')
    else:
        safe_write(ser, serial_lock_out, 'OPEN_GATE')
    print(f"[MANUAL] Sent OPEN_GATE to Arduino {gate.upper()}")

def close_gate(gate='in'):
    ser = arduino_in if gate == 'in' else arduino_out
    if gate == 'in':
        safe_write(ser, serial_lock_in, 'CLOSE_GATE')
    else:
        safe_write(ser, serial_lock_out, 'CLOSE_GATE')
    print(f"[MANUAL] Sent CLOSE_GATE to Arduino {gate.upper()}")


# ==== LUỒNG NỀN: OCR + GIAO TIẾP ====
def camera_loop():
    global plate_counter_in, last_plate_in, last_seen_time_in
    global plate_counter_out, last_plate_out, last_seen_time_out

    while True:
        ret_in, frame_in = cap_in.read()
        ret_out, frame_out = cap_out.read()
        if not ret_in or not ret_out:
            continue

        frame_in, plate_counter_in, last_plate_in, last_seen_time_in = process_frame(
            frame_in, plate_counter_in, last_plate_in, last_seen_time_in, "IN:", arduino_in, serial_lock_in
        )
        frame_out, plate_counter_out, last_plate_out, last_seen_time_out = process_frame(
            frame_out, plate_counter_out, last_plate_out, last_seen_time_out, "OUT:", arduino_out, serial_lock_out
        )

        cv2.imwrite("static/cam_in.jpg", frame_in)
        cv2.imwrite("static/cam_out.jpg", frame_out)

        read_from_arduino(arduino_in, ser_out=arduino_out)
        read_from_arduino(arduino_out)

        time.sleep(0.1)


# ==== KHỞI TẠO FLASK ====
app = Flask(__name__)

# ==== TRANG LOGIN ====
@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        username = request.form.get("username")
        password = request.form.get("password")

        # Tài khoản mặc định (bạn có thể lưu DB sau)
        if username == "admin" and password == "123":
            session["user"] = username
            return redirect("/dashboard")

        return render_template("login.html", error="Sai tài khoản hoặc mật khẩu!")

    return render_template("login.html")


# ==== ĐĂNG XUẤT ====
@app.route("/logout")
def logout():
    session.clear()
    return redirect("/login")

# ==== YÊU CẦU ĐĂNG NHẬP ====
@app.before_request
def require_login():
    open_paths = ["/login", "/static/", "/api/webhook-receiver"]
    path = request.path

    # Cho phép webhook & static
    if any(path.startswith(p) for p in open_paths):
        return

    # Chặn khi chưa login
    if "user" not in session:
        return redirect("/login")


@app.route("/")
def index():
    return render_template("index.html")

@app.route("/rfid")
def rfid_page():
    # đảm bảo file templates/rfid.html tồn tại
    return render_template("rfid.html")

@app.route("/logs")
def get_logs():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT time, uid, name, plate, direction FROM logs ORDER BY id DESC LIMIT 20")
    data = c.fetchall()
    conn.close()
    return jsonify(data)

@app.route("/gate/<direction>/<action>", methods=["POST"])
def control_gate(direction, action):
    if direction not in ["in", "out"] or action not in ["open", "close"]:
        return jsonify({"error": "Invalid command"}), 400
    if action == "open":
        open_gate(direction)
    else:
        close_gate(direction)
    return jsonify({"status": f"Gate {direction} {action}ed successfully"})

@app.route('/led/toggle', methods=['POST'])
def toggle_led():
    safe_write(arduino_out, serial_lock_out, 'TOGGLE_LED')
    print("[WEB] Sent TOGGLE_LED to Arduino OUT")
    return jsonify({"status": "LED toggled"})

@app.route("/sessions")
def view_sessions():
    uid = request.args.get("uid", "").strip()
    plate = request.args.get("plate", "").strip()
    user_type = request.args.get("type", "").strip()
    date_from = request.args.get("from", "")
    date_to = request.args.get("to", "")

    query = "SELECT * FROM sessions WHERE 1=1"
    params = []

    if uid:
        query += " AND uid LIKE ?"
        params.append(f"%{uid}%")
    if plate:
        query += " AND plate LIKE ?"
        params.append(f"%{plate}%")
    if user_type == "resident":
        query += " AND name NOT LIKE 'Guest%'"
    elif user_type == "guest":
        query += " AND name LIKE 'Guest%'"
    if date_from:
        query += " AND date(start_time) >= date(?)"
        params.append(date_from)
    if date_to:
        query += " AND date(end_time) <= date(?)"
        params.append(date_to)

    query += " ORDER BY id DESC"

    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute(query, params)
    data = c.fetchall()
    conn.close()

    return render_template("sessions.html", sessions=data, filters={
        "uid": uid, "plate": plate, "type": user_type,
        "from": date_from, "to": date_to
    })


# === API nhận webhook từ SePay (QR chuyển khoản) ===
@app.route("/api/webhook-receiver", methods=["POST"])
def webhook_receiver():
    data = request.json
    print("[WEBHOOK] Nhận dữ liệu:", data)

    amount = data.get("amount")
    description = data.get("description", "")
    status = data.get("status")

    # Tìm biển số xe trong phần mô tả (VD: "Thanh toan tien gui xe 30A-12345")
    import re
    plate = None
    match = re.search(r"\b\d{2}[A-Z]-?\d{4,5}\b", description)
    if match:
        plate = match.group(0)

    # Nếu webhook báo hoàn tất thì cập nhật trạng thái đã chuyển khoản
    if status == "completed" and plate:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()
        c.execute("""
            UPDATE sessions
            SET status='PAID_QR', fee=? 
            WHERE plate=? AND status='OUT'
            ORDER BY id DESC LIMIT 1
        """, (amount, plate))
        conn.commit()
        conn.close()
        print(f"[DB] 💳 Xe {plate} đã chuyển khoản {amount}đ")
    
    return jsonify({"success": True, "message": "Webhook OK"}), 200


# === API xác nhận thanh toán tiền mặt ===
@app.route("/api/pay/cash/<plate>", methods=["POST"])
def pay_cash(plate):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    # Tìm bản ghi mới nhất đang OUT
    c.execute("""
        SELECT id FROM sessions
        WHERE plate=? AND status='OUT'
        ORDER BY id DESC LIMIT 1
    """, (plate,))
    row = c.fetchone()

    if row:
        session_id = row[0]
        c.execute("UPDATE sessions SET status='PAID_CASH' WHERE id=?", (session_id,))
        conn.commit()
        msg = f"💵 Xe {plate} đã thanh toán tiền mặt"
        print("[DB]", msg)
        conn.close()
        return jsonify({"success": True, "message": msg}), 200
    else:
        conn.close()
        return jsonify({"success": False, "message": f"Không tìm thấy phiên gửi xe đang chờ thanh toán cho {plate}"}), 404

@app.route("/api/pay/qr/<plate>", methods=["POST"])
def pay_qr_manual(plate):
    try:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()

        # Lấy id mới nhất của xe này
        c.execute("""
            SELECT id FROM sessions 
            WHERE plate=? AND status='OUT'
            ORDER BY id DESC LIMIT 1
        """, (plate,))
        row = c.fetchone()

        if row:
            session_id = row[0]
            c.execute("UPDATE sessions SET status='PAID_QR' WHERE id=?", (session_id,))
            conn.commit()
            msg = f"✅ Đã xác nhận chuyển khoản cho xe {plate}"
            print("[DB]", msg)
            return jsonify({"success": True, "message": msg}), 200
        else:
            return jsonify({"success": False, "message": f"Không tìm thấy phiên gửi hợp lệ cho xe {plate}"}), 404

    except Exception as e:
        print("[ERROR]", e)
        return jsonify({"success": False, "message": f"Lỗi máy chủ: {e}"}), 500
    finally:
        conn.close()




@app.route("/dashboard")
def dashboard():
    return render_template("dashboard.html")

@app.route("/api/dashboard-data")
def dashboard_data():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    # 1. Đếm xe đang ở trong bãi
    c.execute("SELECT COUNT(*) FROM sessions WHERE status='IN'")
    in_count = c.fetchone()[0]

    # 2. Đếm xe đã ra
    c.execute("SELECT COUNT(*) FROM sessions WHERE status IN ('OUT','PAID_CASH','PAID_QR')")
    out_count = c.fetchone()[0]

    # 3. Doanh thu hôm nay
    c.execute("SELECT IFNULL(SUM(fee),0) FROM sessions WHERE date(end_time)=date('now')")
    revenue_today = c.fetchone()[0]

    # ===== HÔM NAY =====
    c.execute("SELECT COUNT(*) FROM sessions WHERE date(start_time)=date('now')")
    in_today = c.fetchone()[0]

    c.execute("""
        SELECT COUNT(*) FROM sessions
        WHERE date(end_time)=date('now')
          AND status IN ('OUT','PAID_CASH','PAID_QR')
    """)
    out_today = c.fetchone()[0]

    # ===== TỔNG =====
    c.execute("SELECT COUNT(*) FROM sessions")
    in_total = c.fetchone()[0]

    c.execute("""
        SELECT COUNT(*) FROM sessions
        WHERE status IN ('OUT','PAID_CASH','PAID_QR')
    """)
    out_total = c.fetchone()[0]

    c.execute("SELECT IFNULL(SUM(fee),0) FROM sessions")
    revenue_total = c.fetchone()[0]


    # 4. Biểu đồ lượt vào (7 ngày)
    c.execute("""
        SELECT date(start_time), COUNT(*) as entries
        FROM sessions
        WHERE date(start_time) >= date('now','-7 day')
        GROUP BY date(start_time)
    """)
    rows = c.fetchall()
    dates = [r[0] for r in rows]
    counts = [r[1] for r in rows]

    # 5. Biểu đồ tròn theo trạng thái thanh toán
    c.execute("""
        SELECT 
            SUM(CASE WHEN status='PAID_CASH' THEN 1 ELSE 0 END),
            SUM(CASE WHEN status='PAID_QR' THEN 1 ELSE 0 END),
            SUM(CASE WHEN status IN ('OUT','IN') THEN 1 ELSE 0 END)
        FROM sessions
    """)
    paid_cash, paid_qr, unpaid = c.fetchone()

    # ===== Chart: lượt RA theo ngày (7 ngày gần nhất) =====
    c.execute("""
        SELECT date(end_time), COUNT(*) as exits
        FROM sessions
        WHERE end_time IS NOT NULL
          AND date(end_time) >= date('now','-7 day')
          AND status IN ('OUT','PAID_CASH','PAID_QR')
        GROUP BY date(end_time)
    """)
    rows_out = c.fetchall()

    out_map = {r[0]: r[1] for r in rows_out}
    out_counts_7d = [out_map.get(d, 0) for d in dates]  # dates là labels của chart vào

    # ===== Chart: doanh thu theo ngày (7 ngày gần nhất) =====
    c.execute("""
        SELECT date(end_time), IFNULL(SUM(fee),0) as revenue
        FROM sessions
        WHERE end_time IS NOT NULL
          AND date(end_time) >= date('now','-7 day')
          AND status IN ('OUT','PAID_CASH','PAID_QR')
        GROUP BY date(end_time)
    """)
    rows_rev = c.fetchall()
    rev_map = {r[0]: r[1] for r in rows_rev}
    revenue_7d = [rev_map.get(d, 0) for d in dates]

    # ===== Chart: tỷ lệ trạng thái phiên =====
    c.execute("""
        SELECT
            SUM(CASE WHEN status='IN' THEN 1 ELSE 0 END) as st_in,
            SUM(CASE WHEN status='OUT' THEN 1 ELSE 0 END) as st_out_unpaid,
            SUM(CASE WHEN status='PAID_CASH' THEN 1 ELSE 0 END) as st_cash,
            SUM(CASE WHEN status='PAID_QR' THEN 1 ELSE 0 END) as st_qr
        FROM sessions
    """)
    st_in, st_out_unpaid, st_cash, st_qr = c.fetchone()




    conn.close()

    total_slots = 4
    available = total_slots - in_count

    return jsonify({
        "in_count": in_count,
        "out_count": out_count,
        "available": available,
        "revenue_today": revenue_today,
        "in_today": in_today,
        "out_today": out_today,
        "in_total": in_total,
        "out_total": out_total,
        "revenue_total": revenue_total,
        "chart_labels": dates,
        "chart_values": counts,
        "pie_data": {
            "cash": paid_cash or 0,
            "qr": paid_qr or 0,
            "unpaid": unpaid or 0
        },
        "chart_out_values": out_counts_7d,
        "chart_revenue_values": revenue_7d,
        "status_pie": {
            "in": st_in or 0,
            "out_unpaid": st_out_unpaid or 0,
            "cash": st_cash or 0,
            "qr": st_qr or 0
        }
    })

@app.route("/api/rfid/list")
def rfid_list():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("""
        SELECT uid, owner_name, plate, user_type, status
        FROM rfid_cards
        ORDER BY id DESC
    """)
    rows = c.fetchall()
    conn.close()

    data = []
    for r in rows:
        data.append({
            "uid": r[0],
            "owner_name": r[1],
            "plate": r[2],
            "user_type": r[3],
            "status": r[4],
        })
    return jsonify(data)

@app.route("/api/rfid/add", methods=["POST"])
def rfid_add():
    uid = request.form.get("uid", "").strip()
    owner_name = request.form.get("owner_name", "").strip()
    plate = request.form.get("plate", "").strip()
    user_type = request.form.get("user_type", "guest").strip()  # resident/guest

    if not uid:
        return jsonify({"success": False, "message": "UID không được để trống"}), 400

    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    try:
        c.execute("""
            INSERT INTO rfid_cards (uid, owner_name, plate, user_type, status)
            VALUES (?, ?, ?, ?, 'active')
        """, (uid, owner_name, plate, user_type))
        conn.commit()

        # Gửi xuống ESP32 (không auto-sync tất cả, chỉ gửi lệnh thay đổi)
        cmd = "ADD|{}|{}|{}|{}|active".format(
            _sanitize_field(uid),
            _sanitize_field(owner_name),
            _sanitize_field(plate),
            _sanitize_field(user_type)
        )
        send_to_both(cmd)

        msg = f"Đã thêm thẻ {uid}"
        print("[RFID]", msg)
        return jsonify({"success": True, "message": msg}), 200

    except sqlite3.IntegrityError:
        # UID UNIQUE rồi
        conn.close()
        return jsonify({"success": False, "message": f"UID {uid} đã tồn tại"}), 409

    except Exception as e:
        conn.close()
        return jsonify({"success": False, "message": f"Lỗi: {e}"}), 500

@app.route("/api/rfid/delete/<uid>", methods=["DELETE"])
def rfid_delete(uid):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    c.execute("DELETE FROM rfid_cards WHERE uid=?", (uid,))
    conn.commit()
    conn.close()

    send_to_both("DELETE|{}".format(_sanitize_field(uid)))

    return jsonify({"success": True, "message": f"🗑 Đã xóa thẻ {uid}"}), 200

@app.route("/api/rfid/disable/<uid>", methods=["POST"])
def rfid_disable(uid):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    c.execute("UPDATE rfid_cards SET status='inactive' WHERE uid=?", (uid,))
    conn.commit()
    conn.close()

    send_to_both("DISABLE|{}".format(_sanitize_field(uid)))

    return jsonify({"success": True, "message": f"🔒 Đã khóa thẻ {uid}"}), 200

@app.route("/api/rfid/enable/<uid>", methods=["POST"])
def rfid_enable(uid):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    c.execute("UPDATE rfid_cards SET status='active' WHERE uid=?", (uid,))
    conn.commit()
    conn.close()

    send_to_both("ENABLE|{}".format(_sanitize_field(uid)))

    return jsonify({"success": True, "message": f"🔓 Đã mở khóa thẻ {uid}"}), 200


@app.route("/api/rfid/update/<uid>", methods=["POST"])
def rfid_update(uid):
    uid = uid.strip()
    owner_name = request.form.get("owner_name", "").strip()
    plate = request.form.get("plate", "").strip()
    user_type = request.form.get("user_type", "guest").strip()

    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    c.execute("SELECT id FROM rfid_cards WHERE uid=?", (uid,))
    row = c.fetchone()
    if not row:
        conn.close()
        return jsonify({"success": False, "message": f"Không tìm thấy thẻ {uid}"}), 404

    c.execute("""
        UPDATE rfid_cards
        SET owner_name=?, plate=?, user_type=?
        WHERE uid=?
    """, (owner_name, plate, user_type, uid))
    conn.commit()
    conn.close()

    cmd = "UPDATE|{}|{}|{}|{}".format(
        _sanitize_field(uid),
        _sanitize_field(owner_name),
        _sanitize_field(plate),
        _sanitize_field(user_type)
    )
    send_to_both(cmd)

    msg = f"Đã cập nhật thẻ {uid}"
    print("[RFID]", msg)
    return jsonify({"success": True, "message": msg}), 200

@app.route("/api/rfid/scan/start", methods=["POST"])
def start_rfid_scan():
    global scan_enabled, scan_start_time, scan_result, last_scanned_uid
    scan_enabled = True
    scan_start_time = time.time()
    scan_result = None
    last_scanned_uid = None

    # ===== BẬT SCAN MODE TRÊN ESP32 =====
    send_to_both("SCAN_ON")

    return jsonify({"ok": True})

@app.route("/api/rfid/scan/poll")
def poll_rfid_scan():
    global scan_enabled, scan_start_time, scan_result, last_scanned_uid

    if not scan_enabled and scan_result is None:
        return jsonify({"status": "idle"})

    if scan_enabled and time.time() - scan_start_time > scan_timeout:
        scan_enabled = False
        scan_result = "timeout"
        send_to_both("SCAN_OFF")

    if scan_result is None:
        return jsonify({"status": "waiting"})

    return jsonify({
        "status": scan_result,
        "uid": last_scanned_uid
    })

@app.route("/api/rfid/scan/cancel", methods=["POST"])
def cancel_rfid_scan():
    global scan_enabled, scan_result, last_scanned_uid
    scan_enabled = False
    scan_result = None
    last_scanned_uid = None

    # tắt scan mode ở ESP32 để quay lại chạy bình thường
    send_to_both("SCAN_OFF")

    return jsonify({"ok": True})



from flask import session, redirect, url_for

app.secret_key = "secret123"  # đặt secret mạnh hơn




# ==== CHẠY TOÀN BỘ HỆ THỐNG ====
if __name__ == "__main__":
    init_db()
    upgrade_db()

    threading.Thread(target=camera_loop, daemon=True).start()  # chạy OCR song song

    def delayed_sync_all_cards():
        try:
            # đợi hệ thống ổn định (serial + thread đọc serial chạy rồi)
            time.sleep(5)

            # Nếu bạn có biến arduino_in / arduino_out thì đợi nó khác None
            # (để sync không bị gửi vào None)
            for _ in range(20):  # tối đa ~10s
                ok_in = ("arduino_in" in globals() and arduino_in is not None)
                ok_out = ("arduino_out" in globals() and arduino_out is not None)
                if ok_in or ok_out:
                    break
                time.sleep(0.5)

            sync_all_rfid_cards_to_esp32()

        except Exception as e:
            print("[BOOT_SYNC][ERROR]", e)

    threading.Thread(target=delayed_sync_all_cards, daemon=True).start()

    app.run(debug=False, host="0.0.0.0", port=5000)

