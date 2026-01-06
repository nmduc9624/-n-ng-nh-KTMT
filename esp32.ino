/************************************************************
 * ESP32 MASTER — UI + PAYMENT + UART + FIRESTORE + SONAR
 ************************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <XPT2046_Touchscreen.h>
#include <ESP32Servo.h>

#include <Adafruit_NeoPixel.h>

// ========== WIFI ==========
const char* WIFI_SSID = "duck_sick";
const char* WIFI_PASS = "123456789";

// ========== FIRESTORE ==========
const char* FIREBASE_PROJECT_ID  = "project-7ca17";
const char* FIREBASE_API_KEY     = "AIzaSyBuheYOi9lVSxH_wvpNCwdiLxh9WjP9UDU";

WiFiClientSecure firestoreClient;
HTTPClient firestoreHttp;

// ========== BIN ID (CHÌA KHÓA ĐỒNG BỘ) ==========
#define BIN_ID "BIN_001"

// status document: bins/{BIN_ID}
String FIRESTORE_DOC_PATH = "bins/" + String(BIN_ID);


// collection để lưu pending payments
const char* FIREBASE_COLLECTION = "pending";

// ========== UART ==========
#define UART_TX 17
#define UART_RX 16
#define BAUD    115200

// ========== SONAR ==========
#define SONAR_TRIG 21
#define SONAR_ECHO 5

volatile bool sensorTriggered = false;

// ========== DISPLAY ==========
#define TFT_SCLK  18
#define TFT_MISO  19
#define TFT_MOSI  23
#define TFT_CS    25
#define TFT_DC    26
#define TFT_RST   27

#define TOUCH_CS  32
#define TOUCH_IRQ 33

#define DEVICE_ID "B1"     // mã máy 2 ký tự

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

int SCREEN_W, SCREEN_H;
String transCode = "";

#define LED_PIN    22
#define NUM_LEDS   24
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

Servo servo1;
Servo servo2;

// ========== BILLING DATA ==========
uint32_t qtyBottle  = 0;
uint32_t qtyCan     = 0;
uint32_t qtyUnknown = 0;

const int servo1Pin = 15;
const int servo2Pin = 4;
// Góc mặc định
int angle1 = 180;
int angle2 = 0;

const uint32_t PRICE_BOTTLE = 200;
const uint32_t PRICE_CAN    = 500;

uint32_t total = 0;

// ========== UI ==========
enum UIScreen {
  UI_WELCOME,
  UI_WAITING,
  UI_RESULT,
  UI_PAYMENT,
  UI_FINAL
};
UIScreen uiState = UI_WELCOME;

struct Button { int x,y,w,h; };
Button btnPayment;

bool syncTimeNTP(uint32_t timeoutMs = 15000) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NTP] WiFi not connected");
    return false;
  }

  setenv("TZ", "UTC0", 1);
  tzset();

  configTime(0, 0,
             "time.cloudflare.com",  // ổn định nhất
             "pool.ntp.org",
             "time.google.com");

  Serial.print("[NTP] Syncing time");
  uint32_t start = millis();
  time_t now = 0;

  do {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  } while (now < 1672531200 && (millis() - start) < timeoutMs);

  Serial.println();

  if (now < 1672531200) {
    Serial.println("[NTP] FAILED");
    return false;
  }

  Serial.println("[NTP] OK");
  return true;
}


String firestoreNowTimestampUTC() {
  time_t now = time(nullptr);
  //now += 7 * 3600;

  struct tm t;
  gmtime_r(&now, &t); // UTC + 7
  char buf[25];
  // RFC3339: YYYY-MM-DDTHH:MM:SSZ
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

void servo1Operate() {

  for (angle1 = 180; angle1 >= 50; angle1 -= 5) {
    servo1.write(angle1);
    delay(20);
  }
  
  for (angle1 = 50; angle1 <= 180; angle1 += 5) {
    servo1.write(angle1);
    delay(20);
  }
}

void servo2Operate() {

  for (angle2 = 0; angle2 <= 130; angle2 += 5) {
    servo2.write(angle2);
    delay(20);
  }

  for (angle2 = 130; angle2 >= 0; angle2 -= 5) {
    servo2.write(angle2);
    delay(20);
  }
}

/* ==========================================================
 *                 RING LED HELPERS
 * ========================================================== */
void ringLedOnWhite(uint8_t brightness = 100) {
  strip.setBrightness(brightness);
  for (int i = 0; i < strip.numPixels(); i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 255)); // trắng
  }
  strip.show();
}

void ringLedOff() {
  strip.clear();
  strip.show();
}

/* ==========================================================
 *          SONAR — đọc khoảng cách (cm)
 * ========================================================== */
long readSonarCM() {
  digitalWrite(SONAR_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(SONAR_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(SONAR_TRIG, LOW);

  long duration = pulseIn(SONAR_ECHO, HIGH, 25000); // timeout 25ms ~ 4m
  if (duration == 0) return -1;

  long distance = duration * 0.034 / 2; // μs → cm
  return distance;
}

/* ==========================================================
 *                    HELPER
 * ========================================================== */
bool touchInButton(Button b) {
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();

  int16_t x = map(p.x, 300, 3800, 0, SCREEN_W);
  int16_t y = map(p.y, 300, 3800, 0, SCREEN_H);

  // Giới hạn biên an toàn
  if (x < 0 || y < 0 || x > SCREEN_W || y > SCREEN_H) return false;

  bool hit =
    (x >= b.x && x <= b.x + b.w &&
     y >= b.y && y <= b.y + b.h);

  if (hit) {
    delay(250); // debounce để tránh double tap
  }

  return hit;
}


String generateTransactionCode() {
  const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String randomPart = "";
  for (int i = 0; i < 6; i++) {
    randomPart += charset[random(0, sizeof(charset) - 1)];
  }
  return String(DEVICE_ID) + randomPart;
}

/* ==========================================================
 *                        UI
 * ========================================================== */
void drawWelcome() {
  uiState = UI_WELCOME;
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.println("Xin chao quy khach!");
  tft.setCursor(10, 140);
  tft.println("Rat vui khi duoc gap ban.");

  qtyBottle = qtyCan = qtyUnknown = 0;
}

void drawWaiting() {
  uiState = UI_WAITING;
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  tft.println("Xin quy khach doi");
  tft.setCursor(10, 150);
  tft.println("giay lat!");
}

void drawResultScreen() {
  uiState = UI_RESULT;

  int16_t w = tft.width();
  int16_t h = tft.height();
  const int16_t pad = 12;

  tft.fillScreen(ST77XX_BLACK);

  // ===== Title =====
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);

  const char *line1 = "Da phan loai xong!";
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(line1, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((w - (int16_t)tw) / 2, 60);
  tft.print(line1);

  // ===== Subtitle =====
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);

  const char *line2 = "Nhan nut de hoan tat";
  tft.getTextBounds(line2, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((w - (int16_t)tw) / 2, 100);
  tft.print(line2);

  // ===== Button (centered) =====
  int16_t btnW = w - 2 * pad;
  if (btnW > 280) btnW = 280;          // giới hạn để nhìn gọn (tuỳ bạn bỏ dòng này)
  int16_t btnH = 60;
  int16_t btnX = (w - btnW) / 2;
  int16_t btnY = h - 90;               // sát đáy vừa đẹp

  btnPayment = { btnX, btnY, btnW, btnH };

  tft.fillRoundRect(btnPayment.x, btnPayment.y, btnPayment.w, btnPayment.h, 10, ST77XX_GREEN);
  tft.drawRoundRect(btnPayment.x, btnPayment.y, btnPayment.w, btnPayment.h, 10, ST77XX_WHITE);

  // ===== Button text centered =====
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);

  const char *btnText = "HOAN THANH";
  tft.getTextBounds(btnText, 0, 0, &x1, &y1, &tw, &th);
  int16_t tx = btnPayment.x + (btnPayment.w - (int16_t)tw) / 2;
  int16_t ty = btnPayment.y + (btnPayment.h - (int16_t)th) / 2;

  // getTextBounds th tính theo baseline, nên đẩy xuống chút cho cân
  tft.setCursor(tx, ty + 2);
  tft.print(btnText);
}

/* ==========================================================
 *              UART — Read Clean RESULT
 * ========================================================== */
String readCleanResult(unsigned long timeout) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (Serial2.available()) {
      String msg = Serial2.readStringUntil('\n');
      msg.trim();
      msg.replace("\r", "");
      msg.replace("\n", "");

      if (msg.length() == 0) continue;
      if (!msg.startsWith("RESULT:")) {
        Serial.print("[Ignore] ");
        Serial.println(msg);
        continue;
      }
      return msg;
    }
  }
  return "";
}

/* ==========================================================
 *             FIRESTORE: UPDATE STATUS
 * ========================================================== */
 void firestoreIncrementBottleCounts() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firestore] WiFi not connected");
    return;
  }

  uint32_t heap = ESP.getFreeHeap();
  Serial.printf("[Heap] Free heap before COMMIT: %lu\n", heap);

  if (heap < 40000) {
    Serial.println("[Firestore] Heap too low, skip COMMIT");
    return;
  }

  String url =
    "https://firestore.googleapis.com/v1/projects/" +
    String(FIREBASE_PROJECT_ID) +
    "/databases/(default)/documents:commit?key=" +
    FIREBASE_API_KEY;

  String json =
    "{"
    "  \"writes\": ["
    "    {"
    "      \"transform\": {"
    "        \"document\": \"projects/" + String(FIREBASE_PROJECT_ID) +
              "/databases/(default)/documents/" + FIRESTORE_DOC_PATH + "\","
    "        \"fieldTransforms\": ["
    "          { \"fieldPath\": \"bottle\",  \"increment\": { \"integerValue\": \"" + "1" + "\" } }"
    "        ]"
    "      }"
    "    }"
    "  ]"
    "}";

  firestoreHttp.begin(firestoreClient, url);
  firestoreHttp.addHeader("Content-Type", "application/json");

  int httpCode = firestoreHttp.POST(json);

  Serial.print("[Firestore] COMMIT status = ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    Serial.println(firestoreHttp.getString());
  } else {
    Serial.println("[Firestore] COMMIT failed");
  }

  firestoreHttp.end();
}

 void firestoreIncrementCanCounts() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firestore] WiFi not connected");
    return;
  }

  uint32_t heap = ESP.getFreeHeap();
  Serial.printf("[Heap] Free heap before COMMIT: %lu\n", heap);

  if (heap < 40000) {
    Serial.println("[Firestore] Heap too low, skip COMMIT");
    return;
  }

  String url =
    "https://firestore.googleapis.com/v1/projects/" +
    String(FIREBASE_PROJECT_ID) +
    "/databases/(default)/documents:commit?key=" +
    FIREBASE_API_KEY;

  String json =
    "{"
    "  \"writes\": ["
    "    {"
    "      \"transform\": {"
    "        \"document\": \"projects/" + String(FIREBASE_PROJECT_ID) +
              "/databases/(default)/documents/" + FIRESTORE_DOC_PATH + "\","
    "        \"fieldTransforms\": ["
    "          { \"fieldPath\": \"can\",     \"increment\": { \"integerValue\": \"" + "1" + "\" } }"
    "        ]"
    "      }"
    "    }"
    "  ]"
    "}";

  firestoreHttp.begin(firestoreClient, url);
  firestoreHttp.addHeader("Content-Type", "application/json");

  int httpCode = firestoreHttp.POST(json);

  Serial.print("[Firestore] COMMIT status = ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    Serial.println(firestoreHttp.getString());
  } else {
    Serial.println("[Firestore] COMMIT failed");
  }

  firestoreHttp.end();
}

void firestoreUpdateStatus() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firestore] WiFi not connected");
    return;
  }

  uint32_t heap = ESP.getFreeHeap();
  Serial.printf("[Heap] Free heap before PATCH: %lu\n", heap);

  if (heap < 30000) {
    Serial.println("[Firestore] Heap too low, skip PATCH");
    return;
  }

  String url =
    "https://firestore.googleapis.com/v1/projects/" +
    String(FIREBASE_PROJECT_ID) +
    "/databases/(default)/documents/" +
    FIRESTORE_DOC_PATH +
    "?updateMask.fieldPaths=isOnline"
    "&updateMask.fieldPaths=updatedAt"
    "&key=" + FIREBASE_API_KEY;

  String nowTs = firestoreNowTimestampUTC();

  String json =
    "{ \"fields\": {"
      "\"isOnline\":  {\"booleanValue\": true},"
      "\"updatedAt\": {\"timestampValue\": \"" + nowTs + "\"}"
    "} }";

  firestoreHttp.begin(firestoreClient, url);
  firestoreHttp.addHeader("Content-Type", "application/json");

  int httpCode = firestoreHttp.sendRequest("PATCH", json);

  Serial.print("[Firestore] PATCH status = ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    Serial.println(firestoreHttp.getString());
  } else {
    Serial.println("[Firestore] PATCH failed");
  }

  firestoreHttp.end();
}

// void firestoreUpdateStatus() {

//   if (WiFi.status() != WL_CONNECTED) {
//     Serial.println("[Firestore] WiFi not connected");
//     return;
//   }

//   // kiểm tra heap trước khi HTTPS
//   uint32_t heap = ESP.getFreeHeap();
//   Serial.printf("[Heap] Free heap before PATCH: %lu\n", heap);

//   if (heap < 40000) {
//     Serial.println("[Firestore] Heap too low, skip PATCH");
//     return;
//   }

//   String url =
//     "https://firestore.googleapis.com/v1/projects/" +
//     String(FIREBASE_PROJECT_ID) +
//     "/databases/(default)/documents/" +
//     FIRESTORE_DOC_PATH +
//     "?key=" + FIREBASE_API_KEY;

//   String nowTs = firestoreNowTimestampUTC();

//   String json =
//     "{ \"fields\": {"
//       "\"binId\":     {\"stringValue\": \"" + String(BIN_ID) + "\"},"
//       "\"bottle\":    {\"integerValue\": \"" + String(qtyBottle) + "\"},"
//       "\"can\":       {\"integerValue\": \"" + String(qtyCan) + "\"},"
//       "\"unknown\":   {\"integerValue\": \"" + String(qtyUnknown) + "\"},"
//       "\"isOnline\":  {\"booleanValue\": true},"
//       "\"updatedAt\": {\"timestampValue\": \"" + nowTs + "\"}"
//     "} }";

//   firestoreHttp.begin(firestoreClient, url);
//   firestoreHttp.addHeader("Content-Type", "application/json");

//   int httpCode = firestoreHttp.sendRequest("POST", json);

//   Serial.print("[Firestore] PATCH status = ");
//   Serial.println(httpCode);

//   if (httpCode > 0) {
//     Serial.println(firestoreHttp.getString());
//   } else {
//     Serial.println("[Firestore] PATCH failed (TLS / heap)");
//   }

//   firestoreHttp.end();
// }



/* ==========================================================
 *     FIRESTORE: TẠO DOCUMENT pending/{transCode}
 * ========================================================== */
void firestoreAddPending() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (transCode.length() == 0) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url =
    "https://firestore.googleapis.com/v1/projects/" +
    String(FIREBASE_PROJECT_ID) +
    "/databases/(default)/documents/" +
    String(FIREBASE_COLLECTION) +
    "?documentId=" + transCode +
    "&key=" + FIREBASE_API_KEY;

  total = qtyBottle * PRICE_BOTTLE + qtyCan * PRICE_CAN;

  if (total > 0) {
    String nowTs = firestoreNowTimestampUTC();

    String json =
      "{ \"fields\": {"
        "\"binId\":          {\"stringValue\": \"" + String(BIN_ID) + "\"},"
        "\"canQuantity\":    {\"integerValue\": \"" + String(qtyCan) + "\"},"
        "\"bottleQuantity\": {\"integerValue\": \"" + String(qtyBottle) + "\"},"
        "\"total\":          {\"integerValue\": \"" + String(total) + "\"},"
        "\"createdAt\":      {\"timestampValue\": \"" + nowTs + "\"}"
      "} }";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(json);

    Serial.print("[Firestore] PENDING POST = ");
    Serial.println(code);
    if (code > 0) Serial.println(http.getString());

    http.end();
  }
}


/* ==========================================================
 *                     PAYMENT SCREEN
 * ========================================================== */
void drawPaymentScreen() {
  uiState = UI_PAYMENT;

  int16_t w = tft.width();
  int16_t h = tft.height();
  const int16_t pad = 10;

  tft.fillScreen(ST77XX_BLACK);

  total = qtyBottle * PRICE_BOTTLE + qtyCan * PRICE_CAN;

  // ===== Header =====
  tft.fillRect(0, 0, w, 34, ST77XX_MAGENTA);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);

  const char *title = "HOA DON";
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((w - (int16_t)tw) / 2, 8);
  tft.print(title);

  // ===== Body =====
  int16_t y = 50;

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);

  tft.setCursor(pad, y);
  tft.printf("Bottle: %lu x %d", (unsigned long)qtyBottle, PRICE_BOTTLE);
  y += 34;

  tft.setCursor(pad, y);
  tft.printf("Can:    %lu x %d", (unsigned long)qtyCan, PRICE_CAN);
  y += 34;

  tft.drawFastHLine(pad, y, w - 2 * pad, ST77XX_WHITE);
  y += 18;

  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(pad, y);
  tft.printf("Tong cong: %lu VND", (unsigned long)total);

  // ===== Transaction code (same line, big) =====
  transCode = generateTransactionCode();

  // vùng gần đáy cho Ma GD + countdown
  int16_t transY = h - 55;
  if (transY < y + 40) transY = y + 40;

  // clear vùng dưới
  tft.fillRect(0, transY - 10, w, h - (transY - 10), ST77XX_BLACK);

  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(pad, transY);

  // Tính số ký tự tối đa 1 dòng với size=2
  // Font mặc định: ~6px/char ở size=1 => size=2 ~12px/char
  int maxChars = (w - 2 * pad) / 12;

  String line = "Ma GD: " + transCode;

  // Nếu dài quá thì rút gọn để vẫn cùng 1 dòng (giữ đầu + cuối)
  if ((int)line.length() > maxChars && maxChars > 10) {
    // chừa "Ma GD: " (6 ký tự + space) ~ 6, phần còn lại cho code
    int remain = maxChars - 6;  // phần cho code + dấu ...
    if (remain < 6) remain = 6;

    int head = (remain - 3) / 2;
    int tail = (remain - 3) - head;

    String codeShort = transCode.substring(0, head) + "..." +
                       transCode.substring(transCode.length() - tail);

    line = "Ma GD: " + codeShort;
  }

  tft.print(line);

  // gọi firestore sau khi đã có transCode
  firestoreAddPending();

  // ===== Countdown =====
  int16_t cdY = h - 25;
  if (cdY < 0) cdY = 0;

  for (int i = 15; i >= 0; i--) {
    tft.fillRect(0, cdY - 2, w, 26, ST77XX_BLACK);

    tft.setTextColor(ST77XX_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(pad, cdY);
    tft.printf("Tro ve sau %d s", i);

    delay(1000);
  }

  drawWelcome();
}


/* ==========================================================
 *                       setup()
 * ========================================================== */
void setup() {
  firestoreClient.setInsecure();
  firestoreClient.setTimeout(15000); // rất quan trọng

  Serial.begin(115200);
  Serial.println("MASTER BOOTING...");

  Serial2.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("UART READY");

  // Cấu hình PWM servo
  servo1.setPeriodHertz(50);  // 50Hz cho servo
  servo2.setPeriodHertz(50);

  servo1.attach(servo1Pin, 500, 2500);  // giới hạn xung 0°–180°
  servo2.attach(servo2Pin, 500, 2500);

  ringLedOff();

  // SONAR
  pinMode(SONAR_TRIG, OUTPUT);
  pinMode(SONAR_ECHO, INPUT);

  // TFT
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
  SPI.setFrequency(20000000);

  tft.init(240, 320);
  tft.setRotation(1);

  SCREEN_W = tft.width();
  SCREEN_H = tft.height();

  ts.begin();
  ts.setRotation(1);

  drawWelcome();

  // WIFI
  Serial.println("[WiFi] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected.");
  delay(1000);
  if (syncTimeNTP()) Serial.println("[NTP] Time synced OK!");
  else Serial.println("[NTP] Time sync FAILED (createdAt may be wrong)");
  time_t now = time(nullptr);
  Serial.printf("[DEBUG] time(nullptr) = %ld\n", (long)now);

  randomSeed(esp_random());


}

/* ==========================================================
 *                       loop()
 * ========================================================== */
void loop() {

  // ======= SONAR CHECK =======
  long dist = readSonarCM();
  if (dist > 0 && dist < 15) {   // ngưỡng phát hiện vật
    Serial.printf("[SONAR] Object detected at %ld cm\n", dist);
    sensorTriggered = true;
    delay(500); // tránh lặp liên tục
  }

  // ======= PHÂN LOẠI =======
  if (sensorTriggered) {
    sensorTriggered = false;

    drawWaiting();

    ringLedOnWhite(100);
    //delay(1000);

    Serial.println("MASTER SENT: START");
    Serial2.println("START");

    String result = readCleanResult(8000);

    Serial.print("MASTER RECEIVED: ");
    Serial.println(result);

    if (result == "RESULT:bottle") {
      qtyBottle++;
      firestoreIncrementBottleCounts();
      servo1Operate();
    }
    if (result == "RESULT:can") {
      qtyCan++;
      firestoreIncrementCanCounts();
      servo2Operate();
    }
   // delay(200);
    firestoreUpdateStatus();
    drawResultScreen();
  }

  // ======= NÚT TỔNG TIỀN =======
  if (uiState == UI_RESULT) {
    if (touchInButton(btnPayment)) {
      total = qtyBottle * PRICE_BOTTLE + qtyCan * PRICE_CAN;
      if (total > 0) {
        ringLedOff();
        drawPaymentScreen();
        //firestoreUpdateStatus();
      // firestoreAddPending();
      } else {
        drawWelcome();
      }
      
    }
  }
}
