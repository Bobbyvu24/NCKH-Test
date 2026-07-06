#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ESP32 servo
#include <ESP32Servo.h>

// ================== PIN MAP (ESP32 OUT) ==================
// IR vật cản (UNO A0) -> ESP32 GPIO34 (input-only)
#define PIN_VATCAN      34

// Button mở barie thủ công (UNO D2) -> ESP32 GPIO16 (RX2)
#define PIN_BTN_MANUAL  16

// Buzzer (UNO D5 nhưng code UNO đang hardcode pin 5) -> ESP32 GPIO14
#define PIN_BUZZER      14

// Fire sensor DO (UNO D7) -> ESP32 GPIO36 (VP, input-only)
#define PIN_FIRE        36

// Servo (UNO D6) -> ESP32 GPIO13
#define PIN_SERVO       13

// RGB (UNO A1/A2/A3) -> ESP32 GPIO25/26/33
#define R_PIN           25
#define G_PIN           26
#define B_PIN           33

// Button đổi màu RGB (UNO D3) -> ESP32 GPIO17 (TX2)
#define BUTTON_PIN      17

// RFID RC522 (SPI VSPI)
#define SS_PIN          5
#define RST_PIN         27
#define SPI_SCK         18
#define SPI_MISO        19
#define SPI_MOSI        23

// LCD I2C
#define I2C_SDA         21
#define I2C_SCL         22

// ================== LCD / RFID / SERVO ==================
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo cua_ra;

// ==== Biến RFID ====
int readsuccess;
byte readcard[4];
char str[32] = "";
String StrUID;

// ===== Scan mode for adding new RFID from website =====
bool scanMode = false;

// ===== Anti-spam UID print =====
String lastUIDPrinted = "";
unsigned long lastUIDTime = 0;

// ==== Biển số OCR từ Python (OUT:) ====
String bienSoOCR = "";

// ==== Cấu trúc quản lý thẻ ====
struct TheXe {
  String uid;
  String hoten;
  String bienso;
  bool laResident;
  bool daGanBienSo;
  bool status;         // true = active, false = bị khóa
};

// ===== Danh sách thẻ tối đa 15 (để tiết kiệm RAM) =====
const int MAX_THE = 15;
TheXe dsThe[MAX_THE];
int soThe = 0; // ban đầu rỗng, thẻ sẽ được add/update từ Python qua Serial

// ================== PWM (LEDC) cho RGB ==================
const int PWM_FREQ = 5000;
const int PWM_RES  = 8;     // 0..255

int ledState = 0;           // 0=off, 1=red, 2=green, 3=blue, 4=yellow, 5=white
bool lastButton = HIGH;

void setColor(int r, int g, int b) {
  ledcWrite(R_PIN, r);
  ledcWrite(G_PIN, g);
  ledcWrite(B_PIN, b);
}

// ================== Helpers ==================
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
  lcd.print("Guest chua gan");
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

void LCD() {
  lcd.setCursor(0, 0);
  lcd.print("     Please     ");
  lcd.setCursor(0, 1);
  lcd.print("Check your card ");
}

void LCD_TRUE() {
  lcd.clear();
  lcd.setCursor(5, 0);
  lcd.print("Goodbye");
  lcd.setCursor(0, 1);
  lcd.print("See you again  ");
}

void LCD_BAO_CHAY() {
  lcd.setCursor(0, 0);
  lcd.print("****Warning*** ");
  lcd.setCursor(0, 1);
  lcd.print("Canh Bao Co Chay");
}

// ================== RFID ==================
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

// ================== Mở cửa ==================
void mo_cua() {
  cua_ra.write(100);   // mở barrier
  LCD_TRUE();
  coiCanhBao(2, 100);

  unsigned long start = millis();
  while (true) {
    if (digitalRead(PIN_VATCAN) == LOW) {
      start = millis();
    }
    if (millis() - start > 3000) break;
  }

  cua_ra.write(10);    // đóng barrier
}

// ================== Xử lý RA ==================
void senddata() {
  readsuccess = getid();
  if (!readsuccess) return;

  // ===== SCAN MODE: chỉ gửi UID về Python, KHÔNG validate thẻ =====
  if (scanMode) {
    // StrUID ĐÃ CÓ SẴN TỪ getid()
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

    // TỰ THOÁT SCAN MODE SAU 1 LẦN QUÉT
    scanMode = false;

    return;   // ❗ CỰC QUAN TRỌNG: KHÔNG CHẠY LOGIC MỞ BARRIER
  }
  // ==============================================================



  bool found = false;
  bienSoOCR.trim();

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

        Serial.print("OCR: '"); Serial.print(bienSoOCR); Serial.print("' ");
        Serial.print("DS: '");  Serial.print(dsThe[i].bienso); Serial.println("'");

        if (bienSoOCR.equals(dsThe[i].bienso)) {
          Serial.println("Resident OK -> Mo cua");
          Serial.println("DATA,DATE,TIME," + StrUID + ',' + dsThe[i].hoten + ',' + bienSoOCR + ",Out");
          mo_cua();
          bienSoOCR = "";
        } else {
          Serial.println("Sai bien so resident khi RA!");
          LCD_SaiBienSo();
          coiCanhBao(2);
        }
      } else {
        // Guest
        if (dsThe[i].daGanBienSo) {
          bienSoOCR.trim();
          dsThe[i].bienso.trim();

          Serial.print("OCR: '"); Serial.print(bienSoOCR); Serial.print("' ");
          Serial.print("DS: '");  Serial.print(dsThe[i].bienso); Serial.println("'");

          if (bienSoOCR.equals(dsThe[i].bienso)) {
            Serial.println("Guest OK -> Mo cua");
            Serial.println("DATA,DATE,TIME," + StrUID + ",Guest," + bienSoOCR + ",Out");
            mo_cua();

            // Reset guest sau khi ra
            dsThe[i].bienso = "";
            dsThe[i].daGanBienSo = false;
            bienSoOCR = "";
          } else {
            Serial.println("Sai bien so guest khi RA!");
            LCD_SaiBienSo();
            coiCanhBao(2);
          }
        } else {
          Serial.println("Guest chua co bien so gan tu luc vao!");
          LCD_GuestChuaGan();
          coiCanhBao(2);
        }
      }
    }
  }

  if (!found) {
    Serial.println("The khong hop le!");
    LCD_SaiThe();
    coiCanhBao(4, 500);
  }
}

// ================== Nút RGB ==================
void handleButton() {
  bool buttonState = digitalRead(BUTTON_PIN);
  if (lastButton == HIGH && buttonState == LOW) {
    ledState = (ledState + 1) % 6;
    switch (ledState) {
      case 0: setColor(0,   0,   0);   break;
      case 1: setColor(255, 0,   0);   break;
      case 2: setColor(0,   255, 0);   break;
      case 3: setColor(0,   0,   255); break;
      case 4: setColor(255, 255, 0);   break;
      case 5: setColor(255, 255, 255); break;
    }
    // bíp báo
    digitalWrite(PIN_BUZZER, HIGH);
    delay(100);
    digitalWrite(PIN_BUZZER, LOW);

    delay(200); // chống dội nút
  }
  lastButton = buttonState;
}

// ================== Nhận biển số / lệnh từ Python ==================
void docBienSoTuPython() {
  static String buffer = "";

  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n') {
      buffer.trim();
      Serial.print("Raw from Python: '");
      Serial.print(buffer);
      Serial.println("'");

      if (buffer.startsWith("OUT:")) {
        bienSoOCR = buffer.substring(4);
        bienSoOCR.trim();
        Serial.println("Camera OUT -> bienSoOCR = '" + bienSoOCR + "'");
      }
      else if (buffer.startsWith("REG:")) {
        int commaIndex = buffer.indexOf(',');
        if (commaIndex > 4) {
          String uid = buffer.substring(4, commaIndex);
          String plate = buffer.substring(commaIndex + 1);
          uid.trim(); plate.trim();

          for (int i = 0; i < soThe; i++) {
            if (dsThe[i].uid == uid) {
              dsThe[i].bienso = plate;
              dsThe[i].daGanBienSo = true;
              Serial.println("Guest " + uid + " da duoc cap bien so " + plate);
            }
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
        if (idx != -1) active = dsThe[idx].status;

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

      // Lệnh thủ công
      else if (buffer.equals("OPEN_GATE")) {
        Serial.println("Nhan lenh mo barie thu cong!");
        cua_ra.write(100);
        digitalWrite(PIN_BUZZER, HIGH);
        delay(100);
        digitalWrite(PIN_BUZZER, LOW);
      }
      else if (buffer.equals("CLOSE_GATE")) {
        Serial.println("Nhan lenh dong barie thu cong!");
        cua_ra.write(10);
        digitalWrite(PIN_BUZZER, HIGH);
        delay(100);
        digitalWrite(PIN_BUZZER, LOW);
      }
      else if (buffer.equals("TOGGLE_LED")) {
        Serial.println("Nhan lenh doi mau LED!");
        ledState = (ledState + 1) % 6;
        switch (ledState) {
          case 0: setColor(0,   0,   0);   break;
          case 1: setColor(255, 0,   0);   break;
          case 2: setColor(0,   255, 0);   break;
          case 3: setColor(0,   0,   255); break;
          case 4: setColor(255, 255, 0);   break;
          case 5: setColor(255, 255, 255); break;
        }
        digitalWrite(PIN_BUZZER, HIGH);
        delay(50);
        digitalWrite(PIN_BUZZER, LOW);
      }

      buffer = "";
    } else {
      buffer += c;
    }
  }
}

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(9600);

  // I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // Servo
  cua_ra.setPeriodHertz(50);
  cua_ra.attach(PIN_SERVO, 500, 2400);
  cua_ra.write(10);

  // Inputs/Outputs
  pinMode(PIN_VATCAN, INPUT);
  pinMode(PIN_BTN_MANUAL, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FIRE, INPUT);

  digitalWrite(PIN_BUZZER, LOW);

  // SPI + RFID (chỉ định rõ chân cho chắc)
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  mfrc522.PCD_Init();
  delay(250);

  LCD();
  delay(250);

  // RGB PWM
  ledcAttach(R_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(G_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(B_PIN, PWM_FREQ, PWM_RES);


  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setColor(0, 0, 0); // tắt ban đầu
}

void loop() {
  docBienSoTuPython();

  // Fire sensor
  if (digitalRead(PIN_FIRE) == LOW) {
    lcd.clear();
    LCD_BAO_CHAY();
    digitalWrite(PIN_BUZZER, HIGH);
    cua_ra.write(100);
  } else {
    if (!scanMode) {
      LCD();
    }
    digitalWrite(PIN_BUZZER, LOW);
  }

  // Nút mở thủ công
  if (digitalRead(PIN_BTN_MANUAL) == HIGH) {
    mo_cua();
  }

  // RFID + OCR
  senddata();

  // RGB button
  handleButton();
}
