#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ESP32 servo: cần cài thư viện ESP32Servo
#include <ESP32Servo.h>

// ====== PIN MAP (ESP32 IN) ======
#define PIN_S1          32
#define PIN_S2          33
#define PIN_S3          34   // input-only
#define PIN_S4          35   // input-only

#define PIN_VATCAN      39   // VN, input-only (IR barrier)
#define PIN_BTN_MANUAL  16   // RX2
#define PIN_RELAY       17   // TX2

#define PIN_BUZZER      14
#define PIN_SERVO       13

#define PIN_FIRE        36   // VP, input-only
#define PIN_LIGHT       26

// RFID RC522 (VSPI)
#define SS_PIN          5
#define RST_PIN         27
#define SPI_SCK         18
#define SPI_MISO        19
#define SPI_MOSI        23

// I2C LCD
#define I2C_SDA         21
#define I2C_SCL         22

LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522(SS_PIN, RST_PIN);

int readsuccess;
byte readcard[4];
char str[32] = "";
String StrUID, user;

// ===== Scan mode =====
bool scanMode = false;

// ===== Anti-spam UID print =====
String lastUIDPrinted = "";
unsigned long lastUIDTime = 0;


Servo cua_vao;

int anhsang, lua = HIGH;
int s1, s2, s3, s4;

// Biển số nhận từ Python (camera IN)
String bienSoOCR = "";

// ===== CẤU TRÚC QUẢN LÝ THẺ =====
struct TheXe {
  String uid;
  String hoten;
  String bienso;
  bool laResident;     // true = cư dân, false = khách
  bool daGanBienSo;    // với guest: đã gán biển số hay chưa
  bool status;         // true = active, false = bị khóa
};

// ===== Danh sách thẻ tối đa 15 (để tiết kiệm RAM) =====
const int MAX_THE = 15;
TheXe dsThe[MAX_THE];
int soThe = 0; // ban đầu rỗng, thẻ sẽ được add/update từ Python qua Serial

bool baiDay() {
  return (s1 == LOW && s2 == LOW && s3 == LOW && s4 == LOW);
}

void coiCanhBao(int soLan, int delayTime = 200) {
  for (int i = 0; i < soLan; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(delayTime);
    digitalWrite(PIN_BUZZER, LOW);
    delay(delayTime);
  }
}

void LCD_SaiBienSo() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sai bien so!");
  lcd.setCursor(0, 1);
  lcd.print("Check again");
}
void LCD_SaiThe() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("The khong hop");
  lcd.setCursor(0, 1);
  lcd.print("Check again");
}
void LCD_GuestChuaGan() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Guest chua co");
  lcd.setCursor(0, 1);
  lcd.print("Bien so");
}

void LCD_TheBiKhoa() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("The bi khoa!");
  lcd.setCursor(0, 1);
  lcd.print("Lien he quan ly");
}

// ===== Quản lý mảng thẻ (tối đa 15) =====
int findIndexByUID(const String &uid) {
  for (int i = 0; i < soThe; i++) {
    if (dsThe[i].uid == uid) return i;
  }
  return -1;
}

void removeAt(int idx) {
  if (idx < 0 || idx >= soThe) return;
  for (int i = idx; i < soThe - 1; i++) {
    dsThe[i] = dsThe[i + 1];
  }
  soThe--;
}

bool parseActive(String s) {
  s.trim();
  s.toLowerCase();
  return (s == "active" || s == "1" || s == "true" || s == "on");
}

bool parseResident(String s) {
  s.trim();
  s.toLowerCase();
  return (s == "resident" || s == "res" || s == "1" || s == "true");
}

// Protocol: ADD|uid|owner|plate|type|status
//           UPDATE|uid|owner|plate|type
//           DELETE|uid
//           DISABLE|uid
//           ENABLE|uid
String getToken(const String &s, int index, char delim = '|') {
  int start = 0;
  for (int i = 0; i < index; i++) {
    int p = s.indexOf(delim, start);
    if (p == -1) return "";
    start = p + 1;
  }
  int end = s.indexOf(delim, start);
  if (end == -1) end = s.length();
  return s.substring(start, end);
}

void upsertCard(const String &uid, const String &hoten, const String &plate, bool laResident, bool active) {
  int idx = findIndexByUID(uid);
  if (idx == -1) {
    if (soThe >= MAX_THE) {
      Serial.println("ERR|FULL");
      return;
    }
    idx = soThe;
    soThe++;
  }

  dsThe[idx].uid = uid;
  dsThe[idx].hoten = hoten;
  dsThe[idx].bienso = plate;
  dsThe[idx].laResident = laResident;
  dsThe[idx].daGanBienSo = (plate.length() > 0);
  dsThe[idx].status = active;
}

void LCD_BAO_CHAY() {
  lcd.setCursor(0, 0);
  lcd.print("****Warning*****");
  lcd.setCursor(0, 1);
  lcd.print("Canh Bao Co Chay");
}

int demChoTrong() {
  int count = 0;
  if (s1 != LOW) count++;
  if (s2 != LOW) count++;
  if (s3 != LOW) count++;
  if (s4 != LOW) count++;
  return count;
}

void LCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("     Welcome    ");
  int slots = demChoTrong();
  lcd.setCursor(0, 1);
  lcd.print("Slots: ");
  lcd.print(slots);
  lcd.print("/4  ");
}

void LCD_TRUE() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("1    2    3    4");
  if (s1 == LOW) { lcd.setCursor(0, 1);  lcd.print("X    "); }
  else          { lcd.setCursor(0, 1);  lcd.print("V    "); }
  if (s2 == LOW) { lcd.setCursor(5, 1);  lcd.print("X    "); }
  else          { lcd.setCursor(5, 1);  lcd.print("V    "); }
  if (s3 == LOW) { lcd.setCursor(10, 1); lcd.print("X    "); }
  else          { lcd.setCursor(10, 1); lcd.print("V    "); }
  if (s4 == LOW) { lcd.setCursor(15, 1); lcd.print("X"); }
  else          { lcd.setCursor(15, 1); lcd.print("V"); }
}

void hienParkingFull() {
  lcd.clear();
  lcd.setCursor(4, 0);
  lcd.print("Welcome");
  lcd.setCursor(2, 1);
  lcd.print("Parking Full");

  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(200);
    digitalWrite(PIN_BUZZER, LOW);
    delay(200);
  }
  delay(2000);
}

// RFID
void array_to_string(byte array[], unsigned int len, char buffer[]) {
  for (unsigned int i = 0; i < len; i++) {
    byte nib1 = (array[i] >> 4) & 0x0F;
    byte nib2 = (array[i] >> 0) & 0x0F;
    buffer[i * 2 + 0] = nib1 < 0xA ? '0' + nib1 : 'A' + nib1 - 0xA;
    buffer[i * 2 + 1] = nib2 < 0xA ? '0' + nib2 : 'A' + nib2 - 0xA;
  }
  buffer[len * 2] = '\0';
}

int getid() {
  if (!mfrc522.PICC_IsNewCardPresent()) return 0;
  if (!mfrc522.PICC_ReadCardSerial())   return 0;

  for (int i = 0; i < 4; i++) {
    readcard[i] = mfrc522.uid.uidByte[i];
    array_to_string(readcard, 4, str);
    StrUID = str;
  }
  mfrc522.PICC_HaltA();
  return 1;
}

// ===== Sensor đọc slot/light/fire =====
void sensor() {
  s1 = digitalRead(PIN_S1);
  s2 = digitalRead(PIN_S2);
  s3 = digitalRead(PIN_S3);
  s4 = digitalRead(PIN_S4);

  anhsang = digitalRead(PIN_LIGHT);
  lua     = digitalRead(PIN_FIRE);

  if (anhsang == HIGH) digitalWrite(PIN_RELAY, HIGH);
  else                digitalWrite(PIN_RELAY, LOW);

  if (lua == LOW) {
    LCD_BAO_CHAY();
    cua_vao.write(90);
    digitalWrite(PIN_BUZZER, HIGH);
  } else {
    digitalWrite(PIN_BUZZER, LOW);
    if (!scanMode) LCD();
  }
}

// ===== Mở cửa =====
void mo_cua() {
  cua_vao.write(90);
  LCD_TRUE();

  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(100);
    digitalWrite(PIN_BUZZER, LOW);
    delay(100);
  }

  unsigned long start = millis();
  while (true) {
    if (digitalRead(PIN_VATCAN) == LOW) {
      start = millis();
    }
    if (millis() - start > 3000) break;
  }
  cua_vao.write(0);
}

// ===== Xử lý quét thẻ =====
void senddata() {
  readsuccess = getid();
  if (!readsuccess) return;

  // ===== SCAN MODE: chỉ gửi UID về Python, KHÔNG validate thẻ =====
  if (scanMode) {
    if (StrUID != lastUIDPrinted || (millis() - lastUIDTime) > 1500) {
      Serial.println("UID:" + StrUID);
      lastUIDPrinted = StrUID;
      lastUIDTime = millis();
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("UID SENT");
    lcd.setCursor(0, 1);
    lcd.print(StrUID.substring(0, 16));

    scanMode = false; // tự thoát scan mode sau 1 lần quét
    return;           // ❗ quan trọng: không chạy validate / mở cửa
  }
  // ==============================================================



  bool found = false;

  for (int i = 0; i < soThe; i++) {
    if (dsThe[i].uid == StrUID) {
      found = true;

      // Thẻ bị khóa -> không mở barie
      if (!dsThe[i].status) {
        Serial.println("The bi khoa!");
        LCD_TheBiKhoa();
        coiCanhBao(3);
        return;
      }

      if (dsThe[i].laResident) {
        bienSoOCR.trim();
        dsThe[i].bienso.trim();

        if (bienSoOCR.equals(dsThe[i].bienso)) {
          if (baiDay()) { hienParkingFull(); return; }

          Serial.println("Resident OK -> Mo cua");
          Serial.print("DATA,DATE,TIME,");
          Serial.print(StrUID); Serial.print(",");
          Serial.print(dsThe[i].hoten); Serial.print(",");
          Serial.print(bienSoOCR); Serial.println(",In");

          mo_cua();
          bienSoOCR = "";
        } else {
          Serial.println("Sai biển số resident!");
          LCD_SaiBienSo();
          coiCanhBao(2);
        }
      } else {
        // Guest
        if (bienSoOCR.length() == 0) {
          Serial.println("Guest chưa có biển số OCR, không mở!");
          LCD_GuestChuaGan();
          coiCanhBao(2);
          return;
        }

        if (!dsThe[i].daGanBienSo) {
          dsThe[i].bienso = bienSoOCR;
          dsThe[i].daGanBienSo = true;

          Serial.println("Guest vao, gan bien so: " + bienSoOCR);
          Serial.println("REG:" + StrUID + "," + bienSoOCR);

          if (baiDay()) { hienParkingFull(); return; }

          Serial.print("DATA,DATE,TIME,");
          Serial.print(StrUID);
          Serial.print(",Guest,");
          Serial.print(bienSoOCR);
          Serial.println(",In");

          mo_cua();
          bienSoOCR = "";
        } else {
          bienSoOCR.trim();
          dsThe[i].bienso.trim();

          if (bienSoOCR.equals(dsThe[i].bienso)) {
            if (baiDay()) { hienParkingFull(); return; }

            Serial.print("DATA,DATE,TIME,");
            Serial.print(StrUID);
            Serial.print(",Guest,");
            Serial.print(bienSoOCR);
            Serial.println(",In");

            mo_cua();
            bienSoOCR = "";
          } else {
            Serial.println("Sai biển số guest!");
            LCD_SaiBienSo();
            coiCanhBao(2);
          }
        }
      }
    }
  }

  if (!found) {
    Serial.println("Thẻ không hợp lệ!");
    LCD_SaiThe();
    coiCanhBao(4, 500);
  }
}

// ===== Đọc biển số / lệnh từ Python =====
void docBienSoTuPython() {
  static String buffer = "";

  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      buffer.trim();

      Serial.print("Raw from Python: '");
      Serial.print(buffer);
      Serial.println("'");

      if (buffer.startsWith("IN:")) {
        bienSoOCR = buffer.substring(3);
        bienSoOCR.trim();
        Serial.println("Camera IN -> bienSoOCR = '" + bienSoOCR + "'");
      }
      else if (buffer.startsWith("DEL:")) {
        String uid = buffer.substring(4);
        uid.trim();
        Serial.println("Nhận yêu cầu xóa UID: " + uid);
        for (int i = 0; i < soThe; i++) {
          if (dsThe[i].uid == uid) {
            dsThe[i].bienso = "";
            dsThe[i].daGanBienSo = false;
            Serial.println("Đã reset thẻ khách " + uid);
          }
        }
      }

      else if (buffer.equals("SCAN_ON")) {
        scanMode = true;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Scan mode ON");
        lcd.setCursor(0, 1);
        lcd.print("Tap RFID card");
        Serial.println("OK|SCAN_ON");
      }
      else if (buffer.equals("SCAN_OFF")) {
        scanMode = false;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("SCAN MODE OFF");
        Serial.println("OK|SCAN_OFF");
      }


      // ===== CRUD thẻ từ Website/Python =====
      else if (buffer.equals("RESET_CARDS")) {
        soThe = 0;          //  reset số lượng thẻ trong mảng        
        Serial.println("OK|RESET_CARDS");
      }

      else if (buffer.startsWith("ADD|")) {
        String uid   = getToken(buffer, 1);
        String name  = getToken(buffer, 2);
        String plate = getToken(buffer, 3);
        String type  = getToken(buffer, 4);
        String st    = getToken(buffer, 5);

        uid.trim(); name.trim(); plate.trim(); type.trim(); st.trim();
        bool isResident = parseResident(type);
        bool active = parseActive(st);
        upsertCard(uid, name, plate, isResident, active);

        Serial.println("OK|ADD|" + uid);
      }
      else if (buffer.startsWith("UPDATE|")) {
        String uid   = getToken(buffer, 1);
        String name  = getToken(buffer, 2);
        String plate = getToken(buffer, 3);
        String type  = getToken(buffer, 4);

        uid.trim(); name.trim(); plate.trim(); type.trim();
        bool isResident = parseResident(type);

        int idx = findIndexByUID(uid);
        bool active = true;
        if (idx != -1) active = dsThe[idx].status; // giữ nguyên trạng thái

        upsertCard(uid, name, plate, isResident, active);
        Serial.println("OK|UPDATE|" + uid);
      }
      else if (buffer.startsWith("DELETE|")) {
        String uid = getToken(buffer, 1);
        uid.trim();
        int idx = findIndexByUID(uid);
        if (idx != -1) {
          removeAt(idx);
          Serial.println("OK|DELETE|" + uid);
        } else {
          Serial.println("ERR|NOT_FOUND");
        }
      }
      else if (buffer.startsWith("DISABLE|")) {
        String uid = getToken(buffer, 1);
        uid.trim();
        int idx = findIndexByUID(uid);
        if (idx != -1) {
          dsThe[idx].status = false;
          Serial.println("OK|DISABLE|" + uid);
        } else {
          Serial.println("ERR|NOT_FOUND");
        }
      }
      else if (buffer.startsWith("ENABLE|")) {
        String uid = getToken(buffer, 1);
        uid.trim();
        int idx = findIndexByUID(uid);
        if (idx != -1) {
          dsThe[idx].status = true;
          Serial.println("OK|ENABLE|" + uid);
        } else {
          Serial.println("ERR|NOT_FOUND");
        }
      }
      else if (buffer.equals("OPEN_GATE")) {
        Serial.println("Nhận lệnh mở barie thủ công từ Python!");
        cua_vao.write(100);
        digitalWrite(PIN_BUZZER, HIGH);
        delay(100);
        digitalWrite(PIN_BUZZER, LOW);
      }
      else if (buffer.equals("CLOSE_GATE")) {
        Serial.println("Nhận lệnh đóng barie thủ công từ Python!");
        cua_vao.write(0);
        digitalWrite(PIN_BUZZER, HIGH);
        delay(100);
        digitalWrite(PIN_BUZZER, LOW);
      }

      buffer = "";
    } else {
      buffer += c;
    }
  }
}

void setup() {
  Serial.begin(9600);

  // I2C: ESP32 cần set SDA/SCL
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();

  // Servo (ESP32Servo)
  cua_vao.setPeriodHertz(50);
  cua_vao.attach(PIN_SERVO, 500, 2400);
  cua_vao.write(0);

  pinMode(PIN_VATCAN, INPUT);      // input-only pin OK
  pinMode(PIN_BTN_MANUAL, INPUT);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  pinMode(PIN_FIRE, INPUT);        // input-only pin OK
  pinMode(PIN_LIGHT, INPUT);

  pinMode(PIN_S1, INPUT);
  pinMode(PIN_S2, INPUT);
  pinMode(PIN_S3, INPUT);
  pinMode(PIN_S4, INPUT);

  digitalWrite(PIN_BUZZER, LOW);

  // SPI + RFID (chỉ định rõ chân cho chắc)
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  mfrc522.PCD_Init();

  Serial.println("CLEARDATA");
  Serial.println("LABEL,Date,Time,RFID UID,USER,Plate,IN/OUT");
  delay(500);
  Serial.println("Scan PICC to see UID...");
  LCD();
}

void loop() {
  docBienSoTuPython();
  sensor();
  senddata();

  if (digitalRead(PIN_BTN_MANUAL) == HIGH) {
    mo_cua();
  }
}