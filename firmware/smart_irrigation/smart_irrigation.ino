#define ENABLE_USER_AUTH
#define ENABLE_FIRESTORE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>        // sends the Telegram messages
#include <time.h>
#include <FirebaseClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// =====================================================
// 1. YOUR SETTINGS  (fill these in)
// =====================================================
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define API_KEY       "YOUR_FIREBASE_API_KEY"
#define USER_EMAIL    "YOUR_DEVICE_ACCOUNT_EMAIL"
#define USER_PASSWORD "YOUR_FIREBASE_PASSWORD"

#define FIREBASE_PROJECT_ID   "smart-irrigation-system-ddd4e"
#define CURRENT_DOCUMENT_PATH "irrigation/BU6dFZs4xNrXGgdJWNle"

// =====================================================
// 1.2 TELEGRAM ALERTS
// =====================================================
// true  = send alerts to your phone
// false = no alerts (use this if anything misbehaves on the day)
#define ENABLE_TELEGRAM true

#define TELEGRAM_BOT_TOKEN "PASTE_YOUR_BOT_TOKEN_HERE"
#define TELEGRAM_CHAT_ID   "PASTE_YOUR_CHAT_ID_HERE"

// =====================================================
// 1.3 RAIN FORECAST (skip watering when rain is coming)
// =====================================================
#define ENABLE_RAIN_SKIP true     // false = ignore the weather
#define RAIN_TEST_MODE   false    // true = pretend rain is coming (for the demo)

const char *RAIN_LAT = "-25.52";  // Soshanguve
const char *RAIN_LON = "28.10";
const int  RAIN_PROB_LIMIT = 60;  // skip watering if the rain chance is 60% or more
const int  RAIN_HOURS = 3;        // look this many hours ahead
const unsigned long RAIN_CHECK_MS = 30UL * 60UL * 1000UL;       // check every 30 min
const unsigned long RAIN_VALID_MS = 3UL * 60UL * 60UL * 1000UL; // trust a forecast for 3 h

// The value of the "mode" field on your website when it is automatic
const char *AUTO_MODE_NAME = "AUTO";

// =====================================================
// 1.5. LCD & DHT SETTINGS
// =====================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);

#define DHT_PIN 23      // Digital pin where your DHT sensor is connected
#define DHT_TYPE DHT11  // Change to DHT22 if you are using a DHT22 sensor
DHT dht(DHT_PIN, DHT_TYPE);

// DHT readings (shown on the LCD and sent to the "irrigation" document only)
float tempC = 0;
float humidity = 0;
bool  dhtValid = false;

// =====================================================
// 2. PINS
// =====================================================
// Wiring:
//   Sensor: 3V3 -> sensor +, GND -> sensor GND, pin 34 -> sensor signal
//   Relay : VIN -> relay VCC, GND -> relay GND, pin 26 -> relay SIG/IN
//   DHT11 : S/OUT -> pin 23, + -> 3V3, - -> GND
const int SOIL_SENSOR_PIN = 34;   // sensor signal (AOUT)
const int RELAY_PIN       = 26;   // relay SIG / IN

// Relay method (tested on this hardware, active-LOW module on 5V):
//   ON  = pin driven LOW
//   OFF = pin set to INPUT (stops driving the relay, so it switches off fully)

// =====================================================
// 3. CALIBRATION
// =====================================================
// true  = only print raw sensor values (pump disabled, no Wi-Fi)
// false = normal operation
#define CALIBRATION_MODE false

// DRY_RAW  = raw reading in dry soil
// WET_RAW  = raw reading in water
int DRY_RAW = 0;       // dry soil reads 0
int WET_RAW = 2700;    // water reads about 2700

// Valid raw range.
const int RAW_MIN_VALID = 0;
const int RAW_MAX_VALID = 4095;

bool sensorValid = false;
int  invalidStreak = 0;
const int INVALID_LIMIT = 3;   // consecutive bad readings before declaring fault

// =====================================================
// 4. SOIL CONDITION + PUMP LOGIC
// =====================================================
const int DRY_BELOW = 30;   // 0  - 29  = DRY
const int WET_FROM  = 80;   // 30 - 79  = MOIST,  80 - 100 = WET

// AUTO mode: pump starts when DRY, stops when MOIST
const int PUMP_ON_BELOW = DRY_BELOW;
const int PUMP_OFF_FROM = WET_FROM;

// Pump safety
const unsigned long MAX_PUMP_RUN_MS = 5UL * 60UL * 1000UL;   // 5 min max run
const unsigned long PUMP_REST_MS    = 2UL * 60UL * 1000UL;   // 2 min rest

// AUTO: if the pump has run this long and the sensor still reads exactly 0,
// the sensor is probably not in the soil, so stop and lock AUTO
const unsigned long NO_RESPONSE_MS  = 60UL * 1000UL;         // 60 seconds

// =====================================================
// 5. SPEED SETTINGS
// =====================================================
const unsigned long SENSOR_INTERVAL    = 500;    // Read every 0.5 seconds
const int           CONFIRM_READINGS   = 20;     // 20 dry readings (10 s) before pump ON
const unsigned long MODE_POLL_INTERVAL = 5000;   // Check website every 5 seconds
const unsigned long FAST_POLL_INTERVAL = 2000;   // MANUAL / TIMER: check website every 2 s
const unsigned long MODE_STALE_MS      = 20000;  // No website contact for 20 s = offline
const unsigned long WIFI_BOOT_TIMEOUT_MS = 15000; // At start-up, give up waiting for Wi-Fi after 15 s

const unsigned long UPLOAD_MIN_GAP = 1000;       // Minimum gap between uploads
const int UPLOAD_MIN_CHANGE = 3;                 // Upload after a 3% change
const unsigned long HEARTBEAT_INTERVAL = 30000;  // Regular update every 30 seconds

const unsigned long HISTORY_INTERVAL = 15000;    // History every 15 seconds (use 60000 after the demo)

// =====================================================
// 6. FIREBASE
// =====================================================
UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
Firestore::Documents Docs;

bool authenticationReported = false;
bool firebaseStarted = false;     // true once Firebase sign-in has been started

// Sensor state
int  rawValue = 0;
int  moisturePercent = 0;
bool sensorReady = false;
int  dryStreak = 0;
int  wetStreak = 0;

// Pump state
bool relayOn = false;
unsigned long pumpOnSince = 0;
unsigned long pumpLockedUntil = 0;
bool firstUploadDone = false;     // first live reading reached the website
bool noResponseLock = false;      // AUTO pump locked: sensor still read 0 after a run

// Website state (cached from Firestore)
bool    cachedPump = false;
String  currentMode = "-";
bool    modeKnown = false;
unsigned long lastModeOk = 0;

// Sync / history flags
bool pumpSyncNeeded = false;
unsigned long lastPumpSyncTry = 0;
bool historyNow = false;

// Timers
unsigned long lastSensorRead = 0;
unsigned long lastModePoll = 0;
unsigned long lastUpload = 0;
unsigned long lastHistory = 0;
int lastUploadedPct = -100;
int lastUploadedCond = -1;

// =====================================================
// 6.4 DEMO SWITCH (BOOT button)
// =====================================================
// Press the BOOT button on the ESP32 board to switch Wi-Fi OFF for a demo,
// press it again to switch Wi-Fi back ON. Do not press it while resetting.
const int DEMO_BUTTON_PIN = 0;     // BOOT button on the ESP32 board
bool demoOffline = false;

void checkDemoButton() {
  static bool lastState = HIGH;
  static unsigned long lastChange = 0;

  bool state = digitalRead(DEMO_BUTTON_PIN);

  if (state != lastState && millis() - lastChange > 50) {
    lastChange = millis();
    lastState = state;

    if (state == LOW) {                 // button pressed
      demoOffline = !demoOffline;

      if (demoOffline) {
        Serial.println("DEMO: Wi-Fi switched OFF");
        WiFi.disconnect();
      } else {
        Serial.println("DEMO: Wi-Fi switched ON");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    }
  }
}

// =====================================================
// 6.5 OFFLINE HISTORY BUFFER
// =====================================================
// History readings are stored here first and sent to Firestore when Wi-Fi is up.
// If Wi-Fi is down they wait here, with their real time, and are sent later.
const int HIST_MAX = 120;                 // about 2 hours at 1 reading per minute

struct HistorySample {
  int    moisture;
  bool   pump;
  char   mode[10];
  time_t takenAt;
  bool   offline;
};

HistorySample histBuf[HIST_MAX];
int histHead = 0;      // oldest waiting reading
int histCount = 0;     // how many readings are waiting
int histFails = 0;     // failed sends in a row for the oldest reading
unsigned long lastFlushTry = 0;
const unsigned long FLUSH_GAP_MS = 1000;   // send 1 saved reading per second

// =====================================================
// 6.6 TIMER MODE (like a phone timer)
// =====================================================
// The website sets TIMER mode, a time (5 - 299 seconds) and "timerRunning".
// The ESP32 runs the pump for that time, then switches it off by itself.
bool cachedTimerRunning = false;     // value read from Firestore
long cachedTimerSeconds = 0;         // value read from Firestore
bool timerActive = false;            // the ESP32 is counting down now
unsigned long timerEndsAtMs = 0;     // when the countdown ends (millis)
bool timerFinishedLatch = false;     // stops a finished timer from restarting
bool timerWritePending = false;      // tell the website the timer has ended

// =====================================================
// 6.7 SCHEDULE MODE (water at a set time of day)
// =====================================================
// The website saves: scheduleEnabled, scheduleTime ("HH:MM"),
// scheduleDurationSeconds (5-299) and scheduleDays ("DAILY" or "1,3,5").
// The ESP32 checks its own clock, so it still waters if Wi-Fi drops.
bool   cachedScheduleEnabled = false;    // values read from Firestore
String cachedScheduleTime    = "";       // e.g. "06:30"
long   cachedScheduleSeconds = 0;        // how long to water
String cachedScheduleDays    = "DAILY";  // "DAILY" or e.g. "1,3,5" (0 = Sunday)

bool scheduleActive = false;             // the ESP32 is watering for the schedule now
unsigned long scheduleEndsAtMs = 0;      // when this scheduled run ends (millis)
long lastScheduleRunDay = -1;            // which day it last ran (so it runs once per day)
bool scheduleRunningReported = true;     // last value of scheduleRunning sent to Firestore

// =====================================================
// 6.8 TELEGRAM ALERT QUEUE
// =====================================================
// Alerts wait here and are sent one at a time while Wi-Fi is up.
// If Wi-Fi is down they wait, and are sent when it comes back.
const int TG_MAX = 6;                      // up to 6 alerts can wait
String tgQueue[TG_MAX];
int tgHead = 0;                            // oldest waiting alert
int tgCount = 0;                           // how many are waiting
int tgFails = 0;                           // failed sends in a row
unsigned long lastTgTry = 0;
const unsigned long TG_GAP_MS = 3000;      // at most one alert every 3 s

// Puts an alert in the waiting list (drops the oldest if it is full)
void queueAlert(const String &text) {
#if ENABLE_TELEGRAM
  if (tgCount == TG_MAX) {
    tgHead = (tgHead + 1) % TG_MAX;
    tgCount--;
  }
  tgQueue[(tgHead + tgCount) % TG_MAX] = text;
  tgCount++;
  Serial.print("Alert queued: ");
  Serial.println(text);
#endif
}

// Sends one message to your phone. Returns true if Telegram accepted it.
bool sendTelegram(const String &text) {
  WiFiClientSecure tgClient;               // its own secure connection
  tgClient.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);                   // give up after 5 s

  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendMessage";
  if (!http.begin(tgClient, url)) return false;
  http.addHeader("Content-Type", "application/json");

  // Message as JSON, so emojis and symbols are sent correctly
  DynamicJsonDocument body(512);
  body["chat_id"] = TELEGRAM_CHAT_ID;
  body["text"] = text;
  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  http.end();

  if (code == 200) return true;
  Serial.print("Telegram send failed, HTTP code: ");
  Serial.println(code);
  return false;
}

// Sends the oldest waiting alert. Returns true if it used the internet.
bool flushAlerts() {
#if ENABLE_TELEGRAM
  if (tgCount == 0) return false;                         // nothing waiting
  if (WiFi.status() != WL_CONNECTED) return false;        // offline
  if (millis() - lastTgTry < TG_GAP_MS) return false;     // too soon

  lastTgTry = millis();

  if (sendTelegram(tgQueue[tgHead])) {
    tgHead = (tgHead + 1) % TG_MAX;                       // sent: remove it
    tgCount--;
    tgFails = 0;
  } else if (++tgFails >= 5) {                            // keeps failing: drop it
    Serial.println("Dropping an alert that keeps failing.");
    tgHead = (tgHead + 1) % TG_MAX;
    tgCount--;
    tgFails = 0;
  }
  return true;
#else
  return false;
#endif
}

// =====================================================
// 6.9 RAIN FORECAST
// =====================================================
bool rainExpected = false;         // last forecast said rain
int  rainChance = -1;              // highest rain chance found (%)
bool rainCheckedOnce = false;      // at least one forecast received
unsigned long lastRainCheck = 0;
unsigned long lastRainOk = 0;      // when the last good forecast arrived

// Last rain values sent to Firestore (so it only writes when something changes)
int  rainChanceReported = -2;      // -2 = nothing sent yet
bool rainSkipReported   = false;

// True when AUTO and SCHEDULE should not water because of rain
bool rainSkipActive() {
#if ENABLE_RAIN_SKIP
  if (RAIN_TEST_MODE) return true;                          // demo: pretend rain
  if (!rainCheckedOnce) return false;                       // no forecast yet
  if (millis() - lastRainOk > RAIN_VALID_MS) return false;  // forecast too old
  return rainExpected;
#else
  return false;
#endif
}

// Asks Open-Meteo for the rain chance. Returns true if it used the internet.
bool checkRainForecast() {
#if ENABLE_RAIN_SKIP
  if (RAIN_TEST_MODE) return false;                         // no need to ask
  if (WiFi.status() != WL_CONNECTED) return false;
  if (lastRainCheck != 0 && millis() - lastRainCheck < RAIN_CHECK_MS) return false;
  lastRainCheck = millis();

  WiFiClientSecure weatherClient;
  weatherClient.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + RAIN_LAT +
               "&longitude=" + RAIN_LON +
               "&hourly=precipitation_probability&forecast_hours=" + String(RAIN_HOURS) +
               "&timezone=Africa%2FJohannesburg";

  if (!http.begin(weatherClient, url)) return true;
  int code = http.GET();
  if (code != 200) {
    Serial.print("Weather check failed, HTTP code: ");
    Serial.println(code);
    http.end();
    lastRainCheck = millis() - RAIN_CHECK_MS + 5UL * 60UL * 1000UL;   // try again in 5 min
    return true;
  }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload)) {
    Serial.println("Weather JSON parsing failed.");
    return true;
  }

  // Highest rain chance in the next RAIN_HOURS hours
  int maxChance = 0;
  JsonArray probs = doc["hourly"]["precipitation_probability"].as<JsonArray>();
  for (JsonVariant v : probs) {
    int p = v | 0;
    if (p > maxChance) maxChance = p;
  }

  bool wasExpected = rainCheckedOnce && rainExpected;
  rainChance = maxChance;
  rainExpected = (maxChance >= RAIN_PROB_LIMIT);
  rainCheckedOnce = true;
  lastRainOk = millis();

  Serial.print("Weather: rain chance next ");
  Serial.print(RAIN_HOURS);
  Serial.print(" h = ");
  Serial.print(maxChance);
  Serial.println(rainExpected ? "% -> skipping AUTO/SCHEDULE watering" : "% -> no rain, watering as normal");

  if (rainExpected && !wasExpected) {
    queueAlert("🌧️ Rain expected (" + String(maxChance) + "% in the next " + String(RAIN_HOURS) +
               " h): AUTO and SCHEDULE watering skipped.");
  } else if (!rainExpected && wasExpected) {
    queueAlert("☀️ No rain expected any more: watering resumes.");
  }
  return true;
#else
  return false;
#endif
}

// =====================================================
// 7. HELPERS
// =====================================================
void authDebug(AsyncResult &result) {
  if (result.isEvent()) {
    Serial.print("Firebase event: ");
    Serial.println(result.eventLog().message());
  }
  if (result.isError()) {
    Serial.print("Firebase authentication error: ");
    Serial.println(result.error().message());
  }
}

// Prints only when the pump actually changes state
void setRelay(bool on) {
  if (on != relayOn) {
    Serial.println(on ? ">>> PUMP TURNED ON" : ">>> PUMP TURNED OFF");
  }

  if (on) {
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);     // relay ON
  } else {
    pinMode(RELAY_PIN, INPUT);        // relay OFF
  }

  if (on && !relayOn) pumpOnSince = millis();
  relayOn = on;
}

int conditionOf(int percent) {
  if (percent < DRY_BELOW) return 0;   // DRY
  if (percent < WET_FROM)  return 1;   // MOIST
  return 2;                            // WET
}

const char *conditionName(int cond) {
  if (cond == 0) return "DRY";
  if (cond == 1) return "MOIST";
  return "WET";
}

// True when the ESP32 has no contact with the website (Wi-Fi off,
// no internet, or not signed in). Then it waters on its own.
bool cloudLost() {
  unsigned long now = millis();
  if (!modeKnown) return now > MODE_STALE_MS;    // never reached it since start-up
  return now - lastModeOk > MODE_STALE_MS;       // no contact for 20 s
}

// The mode the ESP32 is really using right now
String effectiveModeName() {
  if (cloudLost() && !currentMode.equalsIgnoreCase("SCHEDULE")) return "AUTO";
  return currentMode;
}

int readMoistureRaw() {
  long total = 0;
  for (int i = 0; i < 10; i++) {
    total += analogRead(SOIL_SENSOR_PIN);
    delayMicroseconds(200);   // short gap between samples
  }
  return total / 10;          // average of 10 samples smooths out noise
}

int calculateMoisturePercent(int raw) {
  if (WET_RAW == DRY_RAW) return 0;
  long pct = (long)(raw - DRY_RAW) * 100 / (WET_RAW - DRY_RAW);
  return constrain(pct, 0, 100);
}

String makeTimestamp() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";

  struct tm utcTime;
  gmtime_r(&now, &utcTime);

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
  return String(buffer);
}

// A number for "today" in South African time (changes at midnight).
// Returns -1 if the clock has not been set from the internet yet.
long localDayNumber() {
  time_t t = time(nullptr);
  if (t < 1700000000) return -1;

  struct tm local;
  localtime_r(&t, &local);                            // SAST (set in setup)
  return (long)local.tm_year * 1000 + local.tm_yday;
}

// True if the schedule should START right now:
// it is enabled, today is a chosen day, the clock shows HH:MM,
// and it has not already run today.
bool scheduleIsDueNow() {
  if (!cachedScheduleEnabled) return false;
  if (cachedScheduleTime.length() != 5) return false;   // must look like "HH:MM"

  long today = localDayNumber();
  if (today < 0) return false;                          // clock not set yet
  if (today == lastScheduleRunDay) return false;        // already ran today

  time_t t = time(nullptr);
  struct tm local;
  localtime_r(&t, &local);

  // Is today one of the chosen days? ("DAILY" means every day)
  if (!cachedScheduleDays.equalsIgnoreCase("DAILY")) {
    char dayChar = '0' + local.tm_wday;                 // '0' = Sunday ... '6' = Saturday
    if (cachedScheduleDays.indexOf(dayChar) < 0) return false;
  }

  // Does the clock show the start time?
  int hh = cachedScheduleTime.substring(0, 2).toInt();  // "06:30" -> 6
  int mm = cachedScheduleTime.substring(3, 5).toInt();  // "06:30" -> 30
  return local.tm_hour == hh && local.tm_min == mm;
}

// =====================================================
// 8. FIRESTORE  (quiet: only prints errors)
// =====================================================
bool uploadReading(int percent) {
  String timestamp = makeTimestamp();
  if (timestamp.length() == 0) {
    Serial.println("Waiting for correct internet time.");
    return false;
  }

  Values::IntegerValue moistureValue(percent);
  Values::TimestampValue timestampValue(timestamp);
  Values::IntegerValue tempValue((int)round(tempC));
  Values::IntegerValue humValue((int)round(humidity));

  Document<Values::Value> document("soilMoisture", Values::Value(moistureValue));
  document.add("timestamp", Values::Value(timestampValue));

  // Only send temperature/humidity when the DHT11 gave a real reading
  String mask = "soilMoisture,timestamp";
  if (dhtValid) {
    document.add("temperature", Values::Value(tempValue));
    document.add("humidity", Values::Value(humValue));
    mask += ",temperature,humidity";
  }

  DocumentMask updateMask(mask.c_str());
  PatchDocumentOptions options(updateMask, DocumentMask(), Precondition());

  Docs.patch(aClient, Firestore::Parent(FIREBASE_PROJECT_ID),
             CURRENT_DOCUMENT_PATH, options, document);

  if (aClient.lastError().code() == 0) return true;

  Serial.print("Upload failed: ");
  Serial.println(aClient.lastError().message());
  return false;
}

bool getPumpAndMode(bool &pump, String &mode, bool &timerRunning, long &timerSeconds) {
  String payload = Docs.get(
    aClient,
    Firestore::Parent(FIREBASE_PROJECT_ID),
    CURRENT_DOCUMENT_PATH,
    GetDocumentOptions(DocumentMask()));   // no mask: read the whole document

  if (aClient.lastError().code() != 0) {
    Serial.print("Failed to read pump/mode: ");
    Serial.println(aClient.lastError().message());
    return false;
  }

  DynamicJsonDocument json(4096);
  if (deserializeJson(json, payload)) {
    Serial.println("JSON parsing failed.");
    return false;
  }

  if (!json["fields"]["pump"]["booleanValue"].is<bool>() ||
      !json["fields"]["mode"]["stringValue"].is<const char *>()) {
    Serial.println("Pump or mode missing from Firestore.");
    return false;
  }

  pump = json["fields"]["pump"]["booleanValue"].as<bool>();
  mode = json["fields"]["mode"]["stringValue"].as<String>();

  // Timer fields are optional: they only exist after the timer is first used
  timerRunning = json["fields"]["timerRunning"]["booleanValue"] | false;

  timerSeconds = 0;
  const char *secondsText = json["fields"]["timerSeconds"]["integerValue"] | "";
  if (secondsText[0] != '\0') {
    timerSeconds = atol(secondsText);
  } else {
    timerSeconds = (long)(json["fields"]["timerSeconds"]["doubleValue"] | 0.0);
  }

  // Schedule fields are optional: they only exist after a schedule is saved
  bool   newEnabled = json["fields"]["scheduleEnabled"]["booleanValue"] | false;
  String newTime    = String(json["fields"]["scheduleTime"]["stringValue"] | "");
  String newDays    = String(json["fields"]["scheduleDays"]["stringValue"] | "DAILY");

  // New time, new days, or switching the schedule back on = a new schedule:
  // forget that it already ran today, so it can run again today
  if (newTime != cachedScheduleTime ||
      newDays != cachedScheduleDays ||
      (newEnabled && !cachedScheduleEnabled)) {
    lastScheduleRunDay = -1;
  }

  cachedScheduleEnabled = newEnabled;
  cachedScheduleTime    = newTime;
  cachedScheduleDays    = newDays;

  // Duration may arrive as a whole number (integerValue, sent as text) or a decimal
  const char *schedText = json["fields"]["scheduleDurationSeconds"]["integerValue"] | "";
  if (schedText[0] != '\0') {
    cachedScheduleSeconds = atol(schedText);
  } else {
    cachedScheduleSeconds = (long)(json["fields"]["scheduleDurationSeconds"]["doubleValue"] | 0.0);
  }

  return true;
}

bool setPumpInFirestore(bool pump) {
  Values::BooleanValue pumpValue(pump);
  Document<Values::Value> document("pump", Values::Value(pumpValue));

  PatchDocumentOptions options(
    DocumentMask("pump"), DocumentMask(), Precondition());

  Docs.patch(aClient, Firestore::Parent(FIREBASE_PROJECT_ID),
             CURRENT_DOCUMENT_PATH, options, document);

  if (aClient.lastError().code() == 0) return true;

  Serial.print("Failed to write pump: ");
  Serial.println(aClient.lastError().message());
  return false;
}

// Tells the website the timer has finished (timerRunning = false)
bool setTimerRunningInFirestore(bool running) {
  Values::BooleanValue runningValue(running);
  Document<Values::Value> document("timerRunning", Values::Value(runningValue));

  PatchDocumentOptions options(
    DocumentMask("timerRunning"), DocumentMask(), Precondition());

  Docs.patch(aClient, Firestore::Parent(FIREBASE_PROJECT_ID),
             CURRENT_DOCUMENT_PATH, options, document);

  if (aClient.lastError().code() == 0) return true;

  Serial.print("Failed to write timer: ");
  Serial.println(aClient.lastError().message());
  return false;
}

// Tells the website whether a scheduled run is happening (scheduleRunning)
bool setScheduleRunningInFirestore(bool running) {
  Values::BooleanValue runningValue(running);
  Document<Values::Value> document("scheduleRunning", Values::Value(runningValue));

  PatchDocumentOptions options(
    DocumentMask("scheduleRunning"), DocumentMask(), Precondition());

  Docs.patch(aClient, Firestore::Parent(FIREBASE_PROJECT_ID),
             CURRENT_DOCUMENT_PATH, options, document);

  if (aClient.lastError().code() == 0) return true;

  Serial.print("Failed to write schedule: ");
  Serial.println(aClient.lastError().message());
  return false;
}

// Tells the website the rain forecast: rainChance, rainSkip, rainTest
bool setRainInFirestore(int chance, bool skip, bool test) {
  Values::IntegerValue chanceValue(chance);
  Values::BooleanValue skipValue(skip);
  Values::BooleanValue testValue(test);

  Document<Values::Value> document("rainChance", Values::Value(chanceValue));
  document.add("rainSkip", Values::Value(skipValue));
  document.add("rainTest", Values::Value(testValue));

  PatchDocumentOptions options(
    DocumentMask("rainChance,rainSkip,rainTest"), DocumentMask(), Precondition());

  Docs.patch(aClient, Firestore::Parent(FIREBASE_PROJECT_ID),
             CURRENT_DOCUMENT_PATH, options, document);

  if (aClient.lastError().code() == 0) return true;

  Serial.print("Failed to write rain: ");
  Serial.println(aClient.lastError().message());
  return false;
}

String timestampFrom(time_t t) {
  struct tm utcTime;
  gmtime_r(&t, &utcTime);

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
  return String(buffer);
}

// Sends one saved reading to Firestore with the time it was really taken
bool sendHistorySample(int moisture, bool pump, const char *modeText, time_t takenAt, bool savedOffline) {
  String timestamp = timestampFrom(takenAt);

  Values::IntegerValue moistureValue(moisture);
  Values::BooleanValue pumpValue(pump);
  String modeString(modeText);
  Values::StringValue modeValue(modeString);
  Values::TimestampValue timestampValue(timestamp);
  Values::BooleanValue offlineValue(savedOffline);

  Document<Values::Value> document("soilMoisture", Values::Value(moistureValue));
  document.add("pump", Values::Value(pumpValue));
  document.add("mode", Values::Value(modeValue));
  document.add("timestamp", Values::Value(timestampValue));

  // Only readings taken while Wi-Fi was down get this tag
  if (savedOffline) {
    document.add("savedOffline", Values::Value(offlineValue));
  }

  Docs.createDocument(aClient, Firestore::Parent(FIREBASE_PROJECT_ID),
                      "irrigationReadings", DocumentMask(), document);

  if (aClient.lastError().code() == 0) return true;

  Serial.print("History save failed: ");
  Serial.println(aClient.lastError().message());
  return false;
}

// Puts a reading in the waiting list (oldest is dropped if the list is full)
void queueHistorySample(int moisture, bool pump, const String &mode, time_t takenAt, bool offline) {
  if (histCount == HIST_MAX) {
    histHead = (histHead + 1) % HIST_MAX;
    histCount--;
  }

  int index = (histHead + histCount) % HIST_MAX;

  histBuf[index].moisture = moisture;
  histBuf[index].pump = pump;
  mode.toCharArray(histBuf[index].mode, sizeof(histBuf[index].mode));
  histBuf[index].takenAt = takenAt;
  histBuf[index].offline = offline;

  histCount++;
}

// Takes a history reading on schedule, whether Wi-Fi is up or not
void recordHistoryIfDue() {
  if (!sensorReady || !sensorValid) return;
  if (!modeKnown && !cloudLost()) return;  // just started: wait for the website's mode

  time_t t = time(nullptr);
  if (t < 1700000000) return;              // clock not set yet

  unsigned long now = millis();

  if (historyNow || lastHistory == 0 || now - lastHistory >= HISTORY_INTERVAL) {
    bool wifiDown = (WiFi.status() != WL_CONNECTED);

    // Offline readings are saved as AUTO, the mode really in use
    queueHistorySample(moisturePercent, relayOn, effectiveModeName(), t, wifiDown);
    lastHistory = now;
    historyNow = false;

    if (wifiDown) {
      Serial.print("Offline - reading saved in memory. Waiting: ");
      Serial.println(histCount);
    }
  }
}

// Sends waiting readings, oldest first, one per second, while Wi-Fi is up.
// Returns true if it used the internet this time, false if nothing to do.
bool flushHistory() {
  if (histCount == 0) return false;                            // nothing waiting
  if (WiFi.status() != WL_CONNECTED) return false;             // offline
  if (millis() - lastFlushTry < FLUSH_GAP_MS) return false;    // too soon

  lastFlushTry = millis();

  HistorySample &oldest = histBuf[histHead];

  if (sendHistorySample(oldest.moisture, oldest.pump, oldest.mode, oldest.takenAt, oldest.offline)) {
    histHead = (histHead + 1) % HIST_MAX;
    histCount--;
    histFails = 0;

    if (histCount > 0) {
      Serial.print("Saved reading sent. Still waiting: ");
      Serial.println(histCount);
    }
  } else {
    histFails++;

    if (histFails >= 5) {                // give up on a reading that keeps failing
      Serial.println("Dropping a reading that keeps failing.");
      histHead = (histHead + 1) % HIST_MAX;
      histCount--;
      histFails = 0;
    }
  }
  return true;
}

void pollModeAndPump() {
  bool pump = false;
  String mode = "";
  bool timerRunning = false;
  long timerSeconds = 0;

  if (getPumpAndMode(pump, mode, timerRunning, timerSeconds)) {
    cachedPump = pump;
    currentMode = mode;
    modeKnown = true;
    lastModeOk = millis();

    cachedTimerRunning = timerRunning;
    cachedTimerSeconds = timerSeconds;

    // The website has cleared the timer, so a new one may start later
    if (!timerRunning) timerFinishedLatch = false;
  }
}

// Tells the website the real pump state.
// Returns true if it used the internet this time, false if nothing to do.
bool syncPumpToWebsite() {
  if (!pumpSyncNeeded) return false;                                          // nothing changed
  if (lastPumpSyncTry != 0 && millis() - lastPumpSyncTry < 3000) return false; // wait before retrying

  lastPumpSyncTry = millis();
  if (setPumpInFirestore(relayOn)) {
    cachedPump = relayOn;
    pumpSyncNeeded = false;
    lastPumpSyncTry = 0;
  }
  return true;                                                                // a Firestore call was made
}

// =====================================================
// 9. FAST PUMP LOGIC  (runs locally, no internet wait)
// =====================================================
void applyPumpLogic() {
  // Only a faulty sensor stops the pump now. Without the website,
  // the ESP32 falls back to AUTO (water only when the soil is dry).
  if (!sensorValid) {
    setRelay(false);
    return;
  }

  unsigned long now = millis();
  bool lost = cloudLost();

  // Which mode is really in use
  bool scheduleMode = currentMode.equalsIgnoreCase("SCHEDULE");            // keeps running offline
  bool timerMode    = currentMode.equalsIgnoreCase("TIMER") && !lost;
  bool autoMode     = currentMode.equalsIgnoreCase(AUTO_MODE_NAME) ||
                      (lost && !scheduleMode);                             // offline fallback
  bool rainSkip     = rainSkipActive();                                    // rain expected?

  // Tell the Serial Monitor when offline AUTO starts or ends
  static bool wasLost = false;
  if (lost != wasLost) {
    wasLost = lost;
    historyNow = true;
    if (lost && !scheduleMode) {
      Serial.println("No contact with the website - OFFLINE AUTO: pump runs only on dry soil.");
    } else if (!lost) {
      Serial.print("Website contact is back - mode: ");
      Serial.println(currentMode);
    }
  }

  // The sensor reads something above 0 again, so it is touching soil or water
  if (rawValue > 0) noResponseLock = false;

  // Pump has run 60 s in AUTO and the sensor still reads exactly 0:
  // probably not in the soil, so stop and lock AUTO
  if (relayOn && autoMode && rawValue == 0 && now - pumpOnSince >= NO_RESPONSE_MS && !noResponseLock) {
    noResponseLock = true;
    Serial.println("Sensor still reads 0 after 60 s of watering - AUTO pump locked. Check the sensor is in the soil.");
    queueAlert("⚠️ Sensor still reads 0 after 60 s of watering. AUTO stopped - check the sensor is in the soil.");
  }

  // ---------- TIMER MODE ----------
  // Leaving TIMER mode, or losing the website, cancels a running timer
  if (!timerMode && timerActive) {
    timerActive = false;
    timerFinishedLatch = true;    // do not restart it when the website comes back
    timerWritePending = true;     // tell the website it has ended
    historyNow = true;
    Serial.println("TIMER cancelled (mode changed or website lost)");
  }

  if (timerMode) {
    // Start a new timer when the website asks for one
    if (cachedTimerRunning && !timerActive && !timerFinishedLatch) {
      long seconds = constrain(cachedTimerSeconds, 5L, 299L);
      timerEndsAtMs = now + (unsigned long)seconds * 1000UL;
      timerActive = true;
      historyNow = true;
      Serial.print("TIMER started: ");
      Serial.print(seconds);
      Serial.println(" s");
    }

    // Cancelled from the website
    if (timerActive && !cachedTimerRunning) {
      timerActive = false;
      Serial.println("TIMER cancelled from the website");
    }

    // Time is up
    if (timerActive && (long)(now - timerEndsAtMs) >= 0) {
      timerActive = false;
      timerFinishedLatch = true;
      timerWritePending = true;
      historyNow = true;
      Serial.println("TIMER finished - pump OFF");
    }
  }

  // ---------- SCHEDULE MODE ----------
  // Leaving SCHEDULE mode, or disabling the schedule, stops a running schedule
  if (scheduleActive && (!scheduleMode || !cachedScheduleEnabled)) {
    scheduleActive = false;
    historyNow = true;
    Serial.println("SCHEDULE stopped (mode changed or schedule disabled)");
  }

  if (scheduleMode) {
    // Start when the clock reaches the saved time on a chosen day
    if (!scheduleActive && scheduleIsDueNow() && rainSkip) {
      lastScheduleRunDay = localDayNumber();   // counts as today's run
      Serial.println("SCHEDULE skipped - rain expected");
      queueAlert("🌧️ Scheduled watering skipped today: rain expected.");
    }

    if (!scheduleActive && scheduleIsDueNow()) {
      long seconds = constrain(cachedScheduleSeconds, 5L, 299L);
      scheduleEndsAtMs = now + (unsigned long)seconds * 1000UL;
      scheduleActive = true;
      lastScheduleRunDay = localDayNumber();   // so it runs once per day
      historyNow = true;
      Serial.print("SCHEDULE started: ");
      Serial.print(seconds);
      Serial.println(" s");
    }

    // Time is up
    if (scheduleActive && (long)(now - scheduleEndsAtMs) >= 0) {
      scheduleActive = false;
      historyNow = true;
      Serial.println("SCHEDULE finished - pump OFF");
    }
  }

  // ---------- DECIDE THE PUMP STATE ----------
  bool wantPump;
  if (scheduleMode) {
    wantPump = scheduleActive;                                      // works offline too
  } else if (autoMode) {
    wantPump = (dryStreak >= CONFIRM_READINGS) && !noResponseLock && !rainSkip;  // AUTO / offline AUTO, not before rain
  } else if (timerMode) {
    wantPump = timerActive;
  } else {
    wantPump = cachedPump;                                          // MANUAL (website reachable)
  }

  // ---------- SAFETY (all modes, online and offline) ----------
  if (relayOn && now - pumpOnSince >= MAX_PUMP_RUN_MS) {
    Serial.println("Max pump run time reached - resting for 2 minutes.");
    queueAlert("⚠️ Pump ran 5 minutes: safety stop, resting 2 minutes.");
    wantPump = false;
    pumpLockedUntil = now + PUMP_REST_MS;

    // A full run and the sensor still reads exactly 0: it is probably not in the soil
    if (autoMode && rawValue == 0) {
      noResponseLock = true;
      Serial.println("Sensor still reads 0 after a full run - AUTO pump locked. Check the sensor is in the soil.");
    }
  }
  if (wantPump && now < pumpLockedUntil) {
    wantPump = false;
  }

  // ---------- SWITCH THE PUMP (and send an alert) ----------
  if (wantPump != relayOn) {
    String modeName = effectiveModeName();
    if (wantPump) {
      queueAlert("💧 Pump ON (" + modeName + "): soil at " + String(moisturePercent) + "%");
    } else {
      unsigned long ranSeconds = (now - pumpOnSince) / 1000;
      queueAlert("✅ Pump OFF (" + modeName + "): soil at " + String(moisturePercent) +
                 "%, ran " + String(ranSeconds) + " s");
    }
    setRelay(wantPump);
    historyNow = true;
  }

  if (relayOn != cachedPump) pumpSyncNeeded = true;
}

// =====================================================
// 10. SETUP
// =====================================================
void setup() {
  pinMode(RELAY_PIN, INPUT);
  pinMode(DEMO_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);

  // Initialize I2C and LCD (SDA 21, SCL 22, Address 0x27)
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Irrigation");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi.");

  // Initialize DHT sensor
  dht.begin();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(SOIL_SENSOR_PIN, INPUT);

#if CALIBRATION_MODE
  Serial.println("CALIBRATION MODE: raw values only, pump disabled");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("CALIBRATION MODE");
  return;
#endif

  Serial.println("\nSmart Irrigation ESP32");
  Serial.println("Connecting to Wi-Fi...");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Wait for Wi-Fi, but not forever
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_BOOT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  ssl_client.setInsecure();

  // South African time; it syncs whenever the internet is available
  configTzTime("SAST-2", "pool.ntp.org", "time.google.com");

  app.getApp<Firestore::Documents>(Docs);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");
    Serial.println("Synchronizing clock...");

    unsigned long startTime = millis();
    while (time(nullptr) < 1700000000 && millis() - startTime < 15000) {
      delay(250);
    }

    Serial.println("Starting Firebase authentication...");
    initializeApp(aClient, app, getAuth(user_auth), authDebug, "authTask");
    firebaseStarted = true;
  } else {
    Serial.println("\nNo Wi-Fi - starting OFFLINE (AUTO: water only when dry).");
    lcd.setCursor(0, 1);
    lcd.print("Offline mode    ");
  }

  queueAlert("🔌 Smart Irrigation started");   // first Telegram alert
#if ENABLE_RAIN_SKIP
  if (RAIN_TEST_MODE) queueAlert("🌧️ Rain test mode ON: AUTO and SCHEDULE will not water.");
#endif
}

// =====================================================
// 11. MAIN LOOP
// =====================================================
void loop() {
#if CALIBRATION_MODE
  Serial.print("RAW: ");
  Serial.println(readMoistureRaw());
  delay(1000);
  return;
#endif

  bool wifiUp = (WiFi.status() == WL_CONNECTED);

  // Started without Wi-Fi? Start Firebase sign-in the first time Wi-Fi appears
  if (wifiUp && !firebaseStarted) {
    Serial.println("Wi-Fi found - starting Firebase authentication...");
    initializeApp(aClient, app, getAuth(user_auth), authDebug, "authTask");
    firebaseStarted = true;
  }

  if (firebaseStarted) app.loop();   // keeps the sign-in alive

  if (app.ready() && !authenticationReported) {
    authenticationReported = true;
    Serial.println("Firebase authentication successful!");
  }

  unsigned long now = millis();

  checkDemoButton();

  // Cloud jobs need Wi-Fi AND a valid sign-in.
  // Without them, the sensor, pump, LCD and history keep working.
  bool online = wifiUp && app.ready();

  static bool wasOnline = true;
  static bool everOnline = false;          // has it been online since start-up?
  static unsigned long offlineSince = 0;   // when the connection dropped
  static unsigned long lastWifiTry = 0;

  if (!wifiUp && !demoOffline && now - lastWifiTry >= 10000) {
    lastWifiTry = now;
    Serial.println("Wi-Fi lost - trying to reconnect...");
    WiFi.reconnect();
  }
  if (!online && wasOnline) {
    offlineSince = now;                    // just went offline
  }
  if (online && !wasOnline) {
    // Only alert after a real drop of 30 s or more (not a quick sign-in refresh)
    if (everOnline && now - offlineSince >= 30000) {
      Serial.println("Online again - sending saved readings.");
      queueAlert("📶 Back online. " + String(histCount) + " saved readings are being uploaded.");
    }
    everOnline = true;
  }
  wasOnline = online;

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    rawValue = readMoistureRaw();

    if (rawValue < RAW_MIN_VALID || rawValue > RAW_MAX_VALID) {
      if (invalidStreak < 1000) invalidStreak++;
      if (invalidStreak >= INVALID_LIMIT) {
        sensorValid = false;
        dryStreak = 0;
        wetStreak = 0;
      }
    } else {
      invalidStreak = 0;
      sensorValid = true;
      moisturePercent = calculateMoisturePercent(rawValue);
      sensorReady = true;

      if (moisturePercent < PUMP_ON_BELOW) { if (dryStreak < 1000) dryStreak++; }
      else dryStreak = 0;

      if (moisturePercent >= PUMP_OFF_FROM) { if (wetStreak < 1000) wetStreak++; }
      else wetStreak = 0;
    }

    static unsigned long lastFaultMsg = 0;
    if (!sensorValid && millis() - lastFaultMsg > 5000) {
      lastFaultMsg = millis();
      Serial.print("SENSOR FAULT: raw = ");
      Serial.print(rawValue);
      Serial.println(" (check wiring/power). Pump locked OFF.");
    }

    applyPumpLogic();

    // Read the DHT11 (it can only update about once every 2 seconds)
    static unsigned long lastDhtRead = 0;
    if (now - lastDhtRead >= 2000) {
      lastDhtRead = now;
      float t = dht.readTemperature();
      float h = dht.readHumidity();

      if (isnan(t) || isnan(h)) {
        dhtValid = false;
      } else {
        tempC = t;
        humidity = h;
        dhtValid = true;
      }
    }

    // LCD: rotate between two screens every 3 seconds
    static unsigned long lastScreenSwitch = 0;
    static int lcdScreen = 0;

    if (now - lastScreenSwitch >= 3000) {
      lastScreenSwitch = now;
      lcdScreen = 1 - lcdScreen;
      lcd.clear();
    }

    if (sensorValid) {
      if (lcdScreen == 0) {
        // Screen 1: moisture, pump and mode
        lcd.setCursor(0, 0);
        lcd.print("Moisture: ");
        lcd.print(moisturePercent);
        lcd.print("%   ");

        lcd.setCursor(0, 1);
        if (timerActive) {
          long left = (long)(timerEndsAtMs - now) / 1000;
          if (left < 0) left = 0;

          char timeText[17];
          snprintf(timeText, sizeof(timeText), "Timer ON  %02ld:%02ld ", left / 60, left % 60);
          lcd.print(timeText);
        } else if (rainSkipActive() && effectiveModeName().equalsIgnoreCase("AUTO")) {
          lcd.print("Rain: no water  ");   // rain expected, AUTO is waiting
        } else if (online) {
          lcd.print("Pump:");
          lcd.print(relayOn ? "ON " : "OFF");
          lcd.print(" ");
          lcd.print(effectiveModeName());   // shows AUTO while watering offline
          lcd.print("    ");
        } else {
          lcd.print("Offline:");
          lcd.print(histCount);
          lcd.print(" saved   ");
        }
      } else {
        // Screen 2: condition, then temperature and humidity
        int cond = conditionOf(moisturePercent);

        lcd.setCursor(0, 0);
        lcd.print("Condition: ");
        lcd.print(conditionName(cond));
        lcd.print("  ");

        lcd.setCursor(0, 1);
        if (dhtValid) {
          lcd.print("Temp:");
          lcd.print((int)tempC);
          lcd.print("C H:");
          lcd.print((int)humidity);
          lcd.print("%  ");
        } else {
          lcd.print("Temp/Hum: --    ");
        }
      }
    } else {
      lcd.setCursor(0, 0);
      lcd.print("SENSOR FAULT    ");
      lcd.setCursor(0, 1);
      lcd.print("Pump: OFF       ");
    }
  }

  // ---------------------------------------------------
  // History: store a reading in memory on schedule.
  // No internet needed here, so it runs every pass.
  // ---------------------------------------------------
  recordHistoryIfDue();

  // ---------------------------------------------------
  // Internet jobs: at most ONE per pass, most urgent first.
  // After one job, loop() goes back to reading the soil and
  // updating the pump, so the pump never waits for several
  // Firestore calls in a row.
  // ---------------------------------------------------
  if (online) {
    bool netUsed = false;   // true once an internet call is made this pass

    // 1) Tell the website the real pump state
    netUsed = syncPumpToWebsite();

    // 1b) Send one waiting Telegram alert
    if (!netUsed) netUsed = flushAlerts();

    // 2) Tell the website the timer has ended
    if (!netUsed && timerWritePending) {
      static unsigned long lastTimerWriteTry = 0;

      if (millis() - lastTimerWriteTry >= 3000) {
        lastTimerWriteTry = millis();
        if (setTimerRunningInFirestore(false)) timerWritePending = false;
        netUsed = true;
      }
    }

    // 3) Check the website's mode, pump button and timer
    if (!netUsed) {
      // MANUAL and TIMER: you are waiting for the pump, so check often.
      // AUTO and SCHEDULE: the ESP32 decides by itself, so check less often.
      bool needFastPoll = currentMode.equalsIgnoreCase("MANUAL") ||
                          currentMode.equalsIgnoreCase("TIMER");

      unsigned long pollEvery = needFastPoll ? FAST_POLL_INTERVAL : MODE_POLL_INTERVAL;

      if (lastModePoll == 0 || millis() - lastModePoll >= pollEvery) {
        lastModePoll = millis();
        pollModeAndPump();
        netUsed = true;
      }
    }

    // 3b) Check the rain forecast (every 30 min)
    if (!netUsed) netUsed = checkRainForecast();

    // 3c) Tell the website the rain forecast (only when it changes)
#if ENABLE_RAIN_SKIP
    if (!netUsed) {
      bool skipNow = rainSkipActive();
      if (rainChance != rainChanceReported || skipNow != rainSkipReported) {
        static unsigned long lastRainWriteTry = 0;
        if (millis() - lastRainWriteTry >= 10000) {         // retry every 10 s if refused
          lastRainWriteTry = millis();
          if (setRainInFirestore(rainChance, skipNow, RAIN_TEST_MODE)) {
            rainChanceReported = rainChance;
            rainSkipReported = skipNow;
          }
          netUsed = true;
        }
      }
    }
#endif

    // 4) Upload the live reading when it changes (or every 30 s)
    if (!netUsed && sensorReady) {
      now = millis();
      int cond = conditionOf(moisturePercent);

      bool changed = abs(moisturePercent - lastUploadedPct) >= UPLOAD_MIN_CHANGE
                     || cond != lastUploadedCond
                     || historyNow;
      bool heartbeat = (now - lastUpload >= HEARTBEAT_INTERVAL);
      bool gapOk = (now - lastUpload >= UPLOAD_MIN_GAP);

      if (lastUpload == 0 || (gapOk && (changed || heartbeat))) {
        lastUpload = now;
        netUsed = true;

        bool uploaded = uploadReading(moisturePercent);

        if (uploaded) {
          lastUploadedPct = moisturePercent;
          lastUploadedCond = cond;
          firstUploadDone = true;
        }

        Serial.println();
        Serial.println("--------------------------------");
        Serial.print("Raw value : "); Serial.println(rawValue);
        Serial.print("Moisture  : "); Serial.print(moisturePercent); Serial.println("%");
        Serial.print("Soil      : "); Serial.println(conditionName(cond));
        if (dhtValid) {
          Serial.print("Temp      : "); Serial.print(tempC); Serial.println(" C");
          Serial.print("Humidity  : "); Serial.print(humidity); Serial.println("%");
        } else {
          Serial.println("Temp/Hum  : no DHT11 reading");
        }
        Serial.print("Pump      : "); Serial.print(relayOn ? "ON" : "OFF");
        Serial.print("   (mode: "); Serial.print(effectiveModeName()); Serial.println(")");
        Serial.print("Cloud     : "); Serial.println(uploaded ? "saved" : "FAILED");
      }
    }

    // 2b) Tell the website whether a scheduled run is happening
    if (!netUsed && scheduleActive != scheduleRunningReported) {
      static unsigned long lastScheduleWriteTry = 0;

      if (millis() - lastScheduleWriteTry >= 3000) {
        lastScheduleWriteTry = millis();
        if (setScheduleRunningInFirestore(scheduleActive)) {
          scheduleRunningReported = scheduleActive;
        }
        netUsed = true;
      }
    }

    // 5) Send saved history readings (least urgent)
    if (!netUsed) {
      flushHistory();
    }
  }

  delay(5);
}
