#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include "time.h"

// =======================================================
// 1️⃣ Network and Firebase settings
// =======================================================
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define FIREBASE_HOST "YOUR_FIREBASE_DATABASE_URL"
#define FIREBASE_AUTH "YOUR_FIREBASE_AUTH_TOKEN"

// time
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10800;
const int   daylightOffset_sec = 0;

// =======================================================
// 2️⃣ Definition of legs
// =======================================================
#define IR_ENTRY_OUT 2
#define IR_EXIT_IN   3
#define IR_SLOT1     4
#define IR_SLOT2     5
#define IR_SLOT3     8
#define SERVO_PIN    18

// LED parking legs
#define LED_P1 20
#define LED_P2 21
#define LED_P3 22

// =======================================================
// 3️⃣ Variables shared between the two kernels
// =======================================================
volatile bool s1_val = false;
volatile bool s2_val = false;
volatile bool s3_val = false;

volatile bool maint_s1 = false;
volatile bool maint_s2 = false;
volatile bool maint_s3 = false;

volatile bool gate_cmd_received = false;

// =======================================================
// 4️⃣ System objects
// =======================================================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo barrierServo;

const int totalSlots = 3;

TaskHandle_t NetworkTask;

// =======================================================
// Helpful functions
// =======================================================
void performGateAction();
void updateLCD(int count);
void NetworkLoop(void * pvParameters);
void logEvent(int slot, String action);

// =======================================================
// 5️⃣ preparation
// =======================================================
void setup() {
  Serial.begin(115200);

  pinMode(IR_ENTRY_OUT, INPUT);
  pinMode(IR_EXIT_IN,   INPUT);
  pinMode(IR_SLOT1,     INPUT);
  pinMode(IR_SLOT2,     INPUT);
  pinMode(IR_SLOT3,     INPUT);

  pinMode(LED_P1, OUTPUT);
  pinMode(LED_P2, OUTPUT);
  pinMode(LED_P3, OUTPUT);
  digitalWrite(LED_P1, LOW);
  digitalWrite(LED_P2, LOW);
  digitalWrite(LED_P3, LOW);

  barrierServo.attach(SERVO_PIN);
  barrierServo.write(0);

  Wire.begin(6, 7);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Starting");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300); Serial.print(".");
  }
  lcd.clear(); lcd.print("WiFi Connected");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(2048);
  Firebase.begin(&config, &auth);

  xTaskCreatePinnedToCore(NetworkLoop, "NetTask", 10000, NULL, 1, &NetworkTask, 0);

  delay(500);
  lcd.clear();
}

// =======================================================
// 🏎️ 6️⃣ loop — Sensors + Gate + Screen + LEDs
// =======================================================
void loop() {
  bool t1 = !digitalRead(IR_SLOT1);
  bool t2 = !digitalRead(IR_SLOT2);
  bool t3 = !digitalRead(IR_SLOT3);

  s1_val = t1;
  s2_val = t2;
  s3_val = t3;

  bool busy1 = (t1 || maint_s1);
  bool busy2 = (t2 || maint_s2);
  bool busy3 = (t3 || maint_s3);

  digitalWrite(LED_P1, busy1 ? HIGH : LOW);
  digitalWrite(LED_P2, busy2 ? HIGH : LOW);
  digitalWrite(LED_P3, busy3 ? HIGH : LOW);

  int occupied = 0;
  if (busy1) occupied++;
  if (busy2) occupied++;
  if (busy3) occupied++;

  int available = totalSlots - occupied;
  if (available < 0) available = 0;

  updateLCD(available);

  if ((digitalRead(IR_ENTRY_OUT) == LOW && available > 0) ||
      (digitalRead(IR_EXIT_IN) == LOW) ||
      gate_cmd_received)
  {
    performGateAction();
    gate_cmd_received = false;
  }

  delay(50);
}

// =======================================================
// 🐢 7️⃣ NetworkLoop — Firebase + Maintenance + Logs
// =======================================================
void NetworkLoop(void * pvParameters) {
  unsigned long lastUpload = 0;

  bool prev_s1 = false;
  bool prev_s2 = false;
  bool prev_s3 = false;

  for (;;) {
    if (Firebase.ready()) {

      if (Firebase.RTDB.getString(&fbdo, "/parking/gate_cmd")) {
        String cmd = fbdo.to<String>();
        if (cmd == "OPEN") {
          gate_cmd_received = true;
          Firebase.RTDB.setString(&fbdo, "/parking/gate_cmd", "NONE");
        }
      }

      if (millis() - lastUpload > 2000) {
        lastUpload = millis();

        bool m1, m2, m3;
        if (Firebase.RTDB.getBool(&fbdo, "/parking/maintenance/slot1", &m1)) maint_s1 = m1;
        if (Firebase.RTDB.getBool(&fbdo, "/parking/maintenance/slot2", &m2)) maint_s2 = m2;
        if (Firebase.RTDB.getBool(&fbdo, "/parking/maintenance/slot3", &m3)) maint_s3 = m3;

        Firebase.RTDB.setBool(&fbdo, "/parking/slots/slot1", s1_val);
        Firebase.RTDB.setBool(&fbdo, "/parking/slots/slot2", s2_val);
        Firebase.RTDB.setBool(&fbdo, "/parking/slots/slot3", s3_val);

        time_t now; time(&now);
        Firebase.RTDB.setInt(&fbdo, "/parking/last_heartbeat", (int)now);

        int occ = 0;
        if (s1_val || maint_s1) occ++;
        if (s2_val || maint_s2) occ++;
        if (s3_val || maint_s3) occ++;
        Firebase.RTDB.setInt(&fbdo, "/parking/available", (totalSlots - occ));
      }

      if (s1_val != prev_s1) {
        logEvent(1, s1_val ? "Enter" : "Exit");
        prev_s1 = s1_val;
      }
      if (s2_val != prev_s2) {
        logEvent(2, s2_val ? "Enter" : "Exit");
        prev_s2 = s2_val;
      }
      if (s3_val != prev_s3) {
        logEvent(3, s3_val ? "Enter" : "Exit");
        prev_s3 = s3_val;
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// =======================================================
// 8️⃣ Program Functions
// =======================================================
void performGateAction() {
  barrierServo.write(90);
  delay(3000);
  barrierServo.write(0);
}

// ✅ Edit here only: If available = 0 → Parking Full
void updateLCD(int count) {
  static int lastCount = -1;
  static bool lastFull = false;

  bool isFull = (count == 0);

 // If neither the number nor the FULL status changes → the screen won't speak
  if (count == lastCount && isFull == lastFull) return;

  lcd.setCursor(0, 0);
  lcd.print("                ");  // Delete line 1
  lcd.setCursor(0, 1);
  lcd.print("                ");  // Delete line 2


  if (isFull) {
    lcd.setCursor(0, 0);
    lcd.print("*** PARKING ***");
    lcd.setCursor(0, 1);
    lcd.print("FULL - NO ENTRY");
  } else {
    lcd.setCursor(0, 0);
    lcd.print("Smart Parking   ");
    lcd.setCursor(0, 1);
    lcd.print("Slots: ");
    lcd.print(count);
    lcd.print("/");
    lcd.print(totalSlots);
    lcd.print("   ");
  }

  lastCount = count;
  lastFull = isFull;
}

void logEvent(int slot, String action) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }

  char dateStr[15];
  strftime(dateStr, 15, "%Y-%m-%d", &timeinfo);

  char timeStr[10];
  strftime(timeStr, 10, "%H:%M:%S", &timeinfo);

  String path = "/logs/";
  path += dateStr;

  FirebaseJson json;
  json.set("slot", slot);
  json.set("action", action);
  json.set("time", timeStr);

  Serial.print("LOG: Slot "); Serial.print(slot);
  Serial.print(" -> "); Serial.println(action);

  Firebase.RTDB.pushJSON(&fbdo, path, &json);
}
