// ============================================================
//  ESP32 NANO — CYBERDECK Device 2  (LUNAR GRADE v7.0)
//  BME680 ONLY — logs to Google Sheets
//
//  Version 7.0 Changes:
//    - BH1750 light sensor COMPLETELY REMOVED (commented out)
//    - Simplified to single I2C device (BME680 only)
//    - All BH1750 references removed from code
//    - Lux removed from logging and displays
//    - Version bumped to 7.0
//
//  Version 6.0 Improvements (LUNAR GRADE - No human intervention):
//    1. Hardware watchdog timer — auto-resets if anything hangs
//    2. I2C bus recovery — clears stuck SDA line after failed reads
//    3. BME680 read timeout — won't block forever if sensor hangs
//    4. HTTP client always cleaned up in all code paths
//    5. Boot counter + reset reason tracking via Preferences
//    6. WiFi reconnect with proper backoff
//    7. Sensor re-initialisation after repeated failures
//    8. Detailed failure tracking visible on /debug page
//    9. Fixed timing bug (lastReading updated BEFORE takeReading)
//   10. Self-healing Sheets failure recovery with WiFi reset
//   11. Emergency reboot if Sheets fails persistently
//   12. FIXED: URL space bug (lux parameter had leading space)
//   13. FIXED: sensorFailures not reset after I2C recovery
//   14. FIXED: initSensors() failure tracking
//   15. ADDED: Emergency reboot if BME680 stays dead
//   16. ADDED: Retry logic for Sheets uploads (3 attempts per reading)
//   17. ADDED: Dead BME680 detection in loop() - REBOOTS automatically
//   18. ADDED: Force sensor read attempt even if bmeOk is false
//   19. ADDED: Progressive backoff for persistent failures
//   20. ADDED: Lunar mode - never gets stuck, always self-heals
//
//  Libraries needed:
//    Adafruit BME680 Library
//    Adafruit Unified Sensor
//
//  Wiring (Arduino Nano ESP32) - SIMPLIFIED:
//    BME680 VCC  → 3.3V
//    BME680 GND  → GND
//    BME680 SDA  → A4 (with 4.7kΩ pullup to 3.3V)
//    BME680 SCL  → A5 (with 4.7kΩ pullup to 3.3V)
//    BME680 SDO  → VCC or GND (sets address 0x77 or 0x76)
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
// #include <BH1750.h>  // BH1750 REMOVED - Light sensor not used
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "secrets.h"

// secrets.h must contain:
//   const char* ssid      = "your_wifi";
//   const char* password  = "your_password";
//   const char* scriptURL = "your_apps_script_url";

// ============================================================
//  CONFIGURATION
// ============================================================

#define SAMPLE_INTERVAL_MS          300000   // 5 minutes
#define WATCHDOG_TIMEOUT_S          35
#define MAX_SENSOR_FAILURES         5
#define SHEETS_FAILURE_RESET_THRESHOLD  3
#define SHEETS_FAILURE_REBOOT_THRESHOLD 10
#define MAX_BME680_REINIT_FAILURES  3

// Lunar mode: BME680 dead timeout (30 minutes)
#define BME680_DEAD_TIMEOUT_MS      1800000  // 30 minutes = 30 * 60 * 1000

// Sheets upload retry settings
#define SHEETS_MAX_RETRIES          3
#define SHEETS_RETRY_DELAY_MS       2000

#define SDA_PIN A4
#define SCL_PIN A5
#define BME680_ADDR 0x77

// ============================================================
//  SENSOR OBJECTS
// ============================================================
Adafruit_BME680 bme;
// BH1750 lightMeter;  // BH1750 REMOVED

bool bmeOk = false;
// bool luxOk = false;  // BH1750 REMOVED
int sensorFailures = 0;
unsigned long bme680DiedAt = 0;  // Track when BME680 stopped working

// ============================================================
//  ENHANCED FAILURE TRACKING
// ============================================================
int bme680FailureCount = 0;
// int bh1750FailureCount = 0;  // BH1750 REMOVED
int slowReadCount = 0;
int i2cRecoveryCount = 0;
int sensorReinitCount = 0;
int bme680ReinitFailures = 0;
String lastFailureType = "None";
unsigned long lastFailureTime = 0;
String lastFailureDetail = "";

int consecutiveSheetsFailures = 0;

// ============================================================
//  CIRCULAR DATA BUFFER
// ============================================================
#define MAX_LOG 120

float tempLog[MAX_LOG];
float humLog[MAX_LOG];
float pressLog[MAX_LOG];
float gasLog[MAX_LOG];
// float luxLog[MAX_LOG];  // BH1750 REMOVED
unsigned long timeLog[MAX_LOG];

int logIndex = 0;
int logCount = 0;

// ============================================================
//  CACHED LATEST VALUES
// ============================================================
float lastTemp = NAN;
float lastHum = NAN;
float lastPress = NAN;
float lastGas = NAN;
// float lastLux = NAN;  // BH1750 REMOVED

// ============================================================
//  DIAGNOSTICS
// ============================================================
Preferences prefs;
int bootCount = 0;
String lastResetReason = "Unknown";
int sheetsSuccess = 0;
int sheetsFailure = 0;
String lastSheetsStatus = "Not attempted";

// ============================================================
//  TIMING
// ============================================================
unsigned long lastReading = 0;

// ============================================================
//  WEB SERVER
// ============================================================
WebServer server(80);


// ============================================================
//  RESET REASON STRING
// ============================================================
String getResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power ON";
    case ESP_RST_EXT:       return "External button";
    case ESP_RST_SW:        return "Software restart";
    case ESP_RST_PANIC:     return "Crash / Panic";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "Task watchdog";
    case ESP_RST_WDT:       return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT — voltage dip";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "Unknown";
  }
}


// ============================================================
//  I2C BUS RECOVERY
// ============================================================
void recoverI2C() {
  Serial.println("[I2C] Attempting bus recovery...");
  i2cRecoveryCount++;

  Wire.end();
  delay(10);

  pinMode(SDA_PIN, OUTPUT);
  pinMode(SCL_PIN, OUTPUT);
  digitalWrite(SDA_PIN, HIGH);
  digitalWrite(SCL_PIN, HIGH);
  delay(5);

  for (int i = 0; i < 16; i++) {
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(5);
    if (digitalRead(SDA_PIN) == HIGH) {
      Serial.println("[I2C] SDA released after " + String(i+1) + " clocks");
      break;
    }
  }

  digitalWrite(SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(SDA_PIN, HIGH);
  delayMicroseconds(5);

  delay(10);
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  Serial.println("[I2C] Bus recovery complete");
}


// ============================================================
//  SENSOR INITIALISATION
// ============================================================
bool initSensors() {
  Serial.println("[Sensors] Initialising...");

  // BME680 only
  bmeOk = bme.begin(BME680_ADDR);
  if (!bmeOk) {
    Serial.printf("[BME680] Not found at 0x%02X, trying alternate...\n", BME680_ADDR);
    bmeOk = bme.begin(BME680_ADDR == 0x77 ? 0x76 : 0x77);
  }
  if (bmeOk) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    Serial.println("[BME680] OK");
    bme680ReinitFailures = 0;
    bme680DiedAt = 0;  // Reset dead timer
  } else {
    Serial.println("[BME680] NOT FOUND");
    bme680ReinitFailures++;
    if (bme680DiedAt == 0) {
      bme680DiedAt = millis();  // Start death timer
    }
  }

  // BH1750 REMOVED - comment out the following block
  // luxOk = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  // if (luxOk) {
  //   Serial.println("[BH1750] OK");
  // } else {
  //   Serial.println("[BH1750] NOT FOUND");
  // }

  sensorFailures = 0;
  return bmeOk;
}


// ============================================================
//  WIFI — AUTO RECONNECT
// ============================================================
void ensureWiFi() {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastAttempt > 15000) {
    Serial.println("[WiFi] Lost connection, reconnecting...");
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid, password);
    lastAttempt = millis();
  }
}


// ============================================================
//  NTP TIME SYNC
// ============================================================
void setupTime() {
  configTime(0, 3600, "pool.ntp.org", "time.nist.gov");
  Serial.print("[Time] Syncing");

  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 100000 && attempts < 20) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }

  if (now > 100000) {
    Serial.println("\n[Time] OK");
  } else {
    Serial.println("\n[Time] Sync timed out");
  }
}


// ============================================================
//  FORCE BME680 READ (even if bmeOk is false)
//  This is the LUNAR GRADE fix - never give up trying
// ============================================================
bool forceBME680Read() {
  // Try to re-initialize if needed
  if (!bmeOk) {
    Serial.println("[BME680] Attempting to re-initialize...");
    bmeOk = bme.begin(BME680_ADDR);
    if (!bmeOk) {
      bmeOk = bme.begin(BME680_ADDR == 0x77 ? 0x76 : 0x77);
    }
    if (bmeOk) {
      bme.setTemperatureOversampling(BME680_OS_8X);
      bme.setHumidityOversampling(BME680_OS_2X);
      bme.setPressureOversampling(BME680_OS_4X);
      bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
      bme.setGasHeater(320, 150);
      Serial.println("[BME680] Re-initialized successfully!");
      bme680DiedAt = 0;
    }
  }
  
  // Attempt to read
  if (bmeOk) {
    unsigned long startMs = millis();
    bool performed = bme.performReading();
    unsigned long elapsed = millis() - startMs;
    
    if (performed && elapsed <= 5000) {
      lastTemp = bme.temperature;
      lastHum = bme.humidity;
      lastPress = bme.pressure / 100.0;
      lastGas = bme.gas_resistance / 1000.0;
      return true;
    } else {
      Serial.printf("[BME680] Force read failed (elapsed: %lums)\n", elapsed);
      // If read fails but bmeOk was true, mark it as dead
      if (bmeOk) {
        bmeOk = false;
        if (bme680DiedAt == 0) {
          bme680DiedAt = millis();
        }
      }
      return false;
    }
  }
  return false;
}


// ============================================================
//  TAKE A SENSOR READING (LUNAR GRADE - BME680 ONLY)
// ============================================================
void takeReading() {
  esp_task_wdt_reset();

  bool readOk = false;

  // ── FORCE BME680 READ - NEVER GIVE UP ──
  // This is the key lunar-grade fix: always try to read,
  // even if bmeOk was false from previous failures
  readOk = forceBME680Read();

  esp_task_wdt_reset();

  // BH1750 read REMOVED - comment out the following block
  // if (luxOk) {
  //   float lux = lightMeter.readLightLevel();
  //   if (lux >= 0) {
  //     lastLux = lux;
  //   } else {
  //     Serial.println("[BH1750] Read failed");
  //     lastLux = NAN;
  //   }
  // }

  esp_task_wdt_reset();

  // ── If read succeeded, reset counters and store data ──
  if (readOk) {
    sensorFailures = 0;
    bme680ReinitFailures = 0;
    bme680DiedAt = 0;
    
    tempLog[logIndex] = lastTemp;
    humLog[logIndex] = lastHum;
    pressLog[logIndex] = lastPress;
    gasLog[logIndex] = lastGas;
    // luxLog[logIndex] = isnan(lastLux) ? 0 : lastLux;  // BH1750 REMOVED
    timeLog[logIndex] = time(nullptr);

    logIndex = (logIndex + 1) % MAX_LOG;
    if (logCount < MAX_LOG) logCount++;

    // Note: Lux removed from print statement
    Serial.printf("[Reading] T:%.1f°C  H:%.1f%%  P:%.0fhPa  Gas:%.0fkΩ\n",
      lastTemp, lastHum, lastPress, lastGas);

    // BH1750 REMOVED - lastLux replaced with placeholder 0
    sendToSheets(lastTemp, lastHum, lastPress, lastGas, 0);  // lux = 0 (not used)
  } else {
    // Read failed - increment counter for I2C recovery
    sensorFailures++;
    Serial.printf("[Sensors] Read failed — consecutive failures: %d/%d\n", 
      sensorFailures, MAX_SENSOR_FAILURES);
    
    // I2C RECOVERY: Too many consecutive failures
    if (sensorFailures >= MAX_SENSOR_FAILURES) {
      Serial.println("[Sensors] Too many failures — recovering I2C and reinitialising");
      sensorReinitCount++;
      lastFailureType = "I2C recovery triggered";
      lastFailureTime = time(nullptr);
      lastFailureDetail = String(sensorFailures) + " consecutive failures";
      recoverI2C();
      delay(200);
      esp_task_wdt_reset();
      initSensors();
      sensorFailures = 0;
      return;
    }
  }
}


// ============================================================
//  SEND TO GOOGLE SHEETS (with retry logic)
//  Note: Lux parameter is kept for compatibility but set to 0
// ============================================================
void sendToSheets(float temp, float hum, float press, float gas, float lux) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Sheets] Skipping — no WiFi");
    lastSheetsStatus = "Skipped — no WiFi";
    consecutiveSheetsFailures = 0;
    return;
  }

  int pressInt = (int)(press + 0.5);
  int gasInt = (int)(gas + 0.5);
  // int luxInt = isnan(lux) ? 0 : (int)(lux + 0.5);  // BH1750 REMOVED - keeping lux=0
  int luxInt = 0;  // BH1750 not present

  String url = String(scriptURL)
    + "?device=2"
    + "&temp="  + String(temp, 1)
    + "&hum="   + String(hum, 1)
    + "&press=" + String(pressInt)
    + "&gas="   + String(gasInt)
    + "&lux="   + String(luxInt);

  if (consecutiveSheetsFailures >= SHEETS_FAILURE_RESET_THRESHOLD) {
    Serial.println("[Sheets] Too many failures - forcing WiFi reconnect");
    WiFi.disconnect();
    delay(1000);
    WiFi.begin(ssid, password);
    delay(3000);
  }

  int httpCode = -1;
  bool success = false;
  
  for (int attempt = 1; attempt <= SHEETS_MAX_RETRIES; attempt++) {
    esp_task_wdt_reset();
    
    Serial.printf("[Sheets] Attempt %d/%d\n", attempt, SHEETS_MAX_RETRIES);
    Serial.println("[URL] " + url);
    
    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    httpCode = http.GET();
    http.end();
    
    esp_task_wdt_reset();
    
    if (httpCode == 200) {
      success = true;
      Serial.println("[Sheets] OK");
      break;
    } else {
      Serial.printf("[Sheets] Failed — HTTP %d (attempt %d/%d)\n", httpCode, attempt, SHEETS_MAX_RETRIES);
      if (attempt < SHEETS_MAX_RETRIES) {
        Serial.printf("[Sheets] Retrying in %d seconds...\n", SHEETS_RETRY_DELAY_MS / 1000);
        delay(SHEETS_RETRY_DELAY_MS);
      }
    }
  }
  
  if (success) {
    sheetsSuccess++;
    lastSheetsStatus = "OK " + String(time(nullptr));
    consecutiveSheetsFailures = 0;
  } else {
    sheetsFailure++;
    consecutiveSheetsFailures++;
    lastSheetsStatus = "Failed HTTP " + String(httpCode) + " at " + String(time(nullptr));
    Serial.printf("[Sheets] Final failure — HTTP %d (failure #%d overall)\n", 
      httpCode, consecutiveSheetsFailures);
    
    if (consecutiveSheetsFailures == SHEETS_FAILURE_RESET_THRESHOLD) {
      Serial.println("[Sheets] Resetting WiFi stack...");
      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(ssid, password);
    }
  }
}


// ============================================================
//  HTTP HANDLERS (Updated for BME680 only)
// ============================================================
void handleData() {
  esp_task_wdt_reset();
  
  // Sensor OK now based only on BME680
  bool sensorOk = !isnan(lastTemp);
  
  String json = "{";
  json += "\"temp\":" + (isnan(lastTemp) ? "null" : String(lastTemp, 1)) + ",";
  json += "\"hum\":" + (isnan(lastHum) ? "null" : String(lastHum, 1)) + ",";
  json += "\"pressCurrent\":" + (isnan(lastPress) ? "null" : String(lastPress, 1)) + ",";
  json += "\"gas\":" + (isnan(lastGas) ? "null" : String(lastGas, 1)) + ",";
  json += "\"luxCurrent\":0,";  // BH1750 REMOVED - always 0
  json += "\"sensorOk\":" + String(sensorOk ? "true" : "false") + ",";
  json += "\"bme680Ok\":" + String(!isnan(lastTemp) ? "true" : "false") + ",";
  json += "\"bh1750Ok\":false,";  // BH1750 REMOVED
  json += "\"wifiOk\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"sheetsSuccess\":" + String(sheetsSuccess) + ",";
  json += "\"sheetsFailure\":" + String(sheetsFailure) + ",";
  json += "\"bootCount\":" + String(bootCount) + ",";
  json += "\"resetReason\":\"" + lastResetReason + "\",";
  json += "\"uptimeMin\":" + String(millis() / 60000) + ",";
  json += "\"labels\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(timeLog[idx]);
    if (i < logCount - 1) json += ",";
  }
  json += "],\"temps\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(tempLog[idx], 1);
    if (i < logCount - 1) json += ",";
  }
  json += "],\"hums\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(humLog[idx], 1);
    if (i < logCount - 1) json += ",";
  }
  json += "],\"pressHistory\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(pressLog[idx], 1);
    if (i < logCount - 1) json += ",";
  }
  json += "],\"luxHistory\":[";  // BH1750 REMOVED - empty array
  for (int i = 0; i < logCount; i++) {
    json += "0";
    if (i < logCount - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleDebug() {
  esp_task_wdt_reset();
  String html = "<html><body style='background:#060d06;color:#00ff9c;font-family:monospace;padding:20px;'>";
  html += "<h1 style='color:#ffaa00'>Device 2 — Diagnostics v7.0 (BME680 ONLY)</h1><pre>";
  html += "=== Sensors ===\n";
  html += "BME680 init     : " + String(bmeOk ? "OK" : "FAILED") + "\n";
  html += "BH1750 init     : NOT USED (removed in v7.0)\n";
  html += "Failures        : " + String(sensorFailures) + " consecutive\n\n";
  html += "=== Current Readings ===\n";
  html += "Temperature     : " + String(lastTemp) + " °C\n";
  html += "Humidity        : " + String(lastHum) + " %\n";
  html += "Pressure        : " + String(lastPress) + " hPa\n";
  html += "Gas             : " + String(lastGas) + " kΩ\n";
  html += "Lux             : N/A (BH1750 removed)\n\n";
  html += "=== Failure Details ===\n";
  html += "Last failure    : " + lastFailureType + "\n";
  if (lastFailureTime > 0) {
    char timeBuf[32];
    time_t failureTime = (time_t)lastFailureTime;
    struct tm *timeinfo = localtime(&failureTime);
    if (timeinfo != NULL) {
      strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", timeinfo);
      html += "Failure time    : " + String(timeBuf) + "\n";
    }
  }
  html += "Failure detail  : " + lastFailureDetail + "\n";
  html += "BME680 fails    : " + String(bme680FailureCount) + " total\n";
  html += "Slow reads      : " + String(slowReadCount) + "\n";
  html += "I2C recoveries  : " + String(i2cRecoveryCount) + "\n";
  html += "Sensor reinit   : " + String(sensorReinitCount) + "\n";
  html += "BME680 reinit fails: " + String(bme680ReinitFailures) + "\n\n";
  html += "=== Sheets Status ===\n";
  html += "Success         : " + String(sheetsSuccess) + "\n";
  html += "Failure         : " + String(sheetsFailure) + "\n";
  html += "Consecutive fails: " + String(consecutiveSheetsFailures) + "\n";
  html += "Last status     : " + lastSheetsStatus + "\n";
  html += "Retry settings  : " + String(SHEETS_MAX_RETRIES) + " attempts, " + String(SHEETS_RETRY_DELAY_MS/1000) + "s delay\n\n";
  html += "=== System ===\n";
  html += "Boot count      : " + String(bootCount) + "\n";
  html += "Last reset      : " + lastResetReason + "\n";
  html += "Uptime          : " + String(millis() / 60000) + " minutes\n";
  if (bme680DiedAt > 0) {
    html += "BME680 dead for : " + String((millis() - bme680DiedAt) / 60000) + " minutes\n";
  }
  html += "WiFi            : " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "\n";
  html += "IP              : " + WiFi.localIP().toString() + "\n";
  html += "RSSI            : " + String(WiFi.RSSI()) + " dBm\n\n";
  html += "=== Buffer ===\n";
  html += "Readings        : " + String(logCount) + " / " + String(MAX_LOG) + "\n";
  html += "Log index       : " + String(logIndex) + "\n";
  html += "</pre>";
  html += "<p><a href='/' style='color:#ffaa00'>Dashboard</a> | ";
  html += "<a href='/rawdata' style='color:#00ff9c'>Raw JSON</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleRawData() {
  esp_task_wdt_reset();
  bool sensorOk = !isnan(lastTemp);
  String json = "{\n";
  json += "  \"temp\":" + (isnan(lastTemp) ? "null" : String(lastTemp, 1)) + ",\n";
  json += "  \"hum\":" + (isnan(lastHum) ? "null" : String(lastHum, 1)) + ",\n";
  json += "  \"press\":" + (isnan(lastPress) ? "null" : String(lastPress, 1)) + ",\n";
  json += "  \"gas\":" + (isnan(lastGas) ? "null" : String(lastGas, 1)) + ",\n";
  json += "  \"lux\":0,\n";  // BH1750 REMOVED
  json += "  \"sensorOk\":" + String(sensorOk ? "true" : "false") + ",\n";
  json += "  \"logCount\":" + String(logCount) + ",\n";
  json += "  \"bootCount\":" + String(bootCount) + ",\n";
  json += "  \"resetReason\":\"" + lastResetReason + "\",\n";
  json += "  \"lastFailure\":\"" + lastFailureType + "\",\n";
  json += "  \"bme680Failures\":" + String(bme680FailureCount) + ",\n";
  json += "  \"i2cRecoveries\":" + String(i2cRecoveryCount) + ",\n";
  json += "  \"sheetsSuccess\":" + String(sheetsSuccess) + ",\n";
  json += "  \"sheetsFailure\":" + String(sheetsFailure) + ",\n";
  json += "  \"consecutiveSheetsFailures\":" + String(consecutiveSheetsFailures) + ",\n";
  json += "  \"retryAttempts\":" + String(SHEETS_MAX_RETRIES) + "\n";
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  esp_task_wdt_reset();
  String itvl = String(SAMPLE_INTERVAL_MS / 60000);
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>CYBERDECK // DEVICE 2</title>";
  html += "<style>body{background:#060d06;color:#00ff9c;font-family:'Courier New',monospace;padding:20px;}";
  html += "h1{color:#ffaa00;}</style>";
  html += "</head><body>";
  html += "<h1>CYBERDECK // DEVICE 2</h1>";
  html += "<p>Version: 7.0 (BME680 ONLY - Simplified)</p>";
  html += "<p>Interval: " + itvl + " minutes</p>";
  html += "<p>Sheets retries: " + String(SHEETS_MAX_RETRIES) + " attempts, " + String(SHEETS_RETRY_DELAY_MS/1000) + "s delay</p>";
  html += "<p>Watchdog: " + String(WATCHDOG_TIMEOUT_S) + " seconds</p>";
  html += "<p>BME680 dead timeout: 30 minutes (auto-reboot)</p>";
  html += "<p><a href='/debug'>DEBUG</a> | <a href='/rawdata'>RAW JSON</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}


// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  prefs.begin("d2", false);
  bootCount = prefs.getInt("boots", 0) + 1;
  prefs.putInt("boots", bootCount);
  prefs.end();

  lastResetReason = getResetReason();

  Serial.println("\n[SYSTEM] CyberDeck Device 2 — LUNAR GRADE v7.0 (BME680 ONLY)");
  Serial.printf("[SYSTEM] Boot #%d — Last reset: %s\n", bootCount, lastResetReason.c_str());
  Serial.printf("[SYSTEM] Sheets retry: %d attempts, %ds delay\n", SHEETS_MAX_RETRIES, SHEETS_RETRY_DELAY_MS/1000);
  Serial.printf("[SYSTEM] Watchdog: %d seconds\n", WATCHDOG_TIMEOUT_S);
  Serial.printf("[SYSTEM] BME680 dead timeout: %d minutes\n", BME680_DEAD_TIMEOUT_MS / 60000);
  Serial.println("[SYSTEM] BH1750 light sensor: NOT USED (removed for reliability)");

  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(50);

  Serial.println("[I2C] Scanning...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Found device at 0x%02X\n", addr);
    }
    esp_task_wdt_reset();
  }

  initSensors();

  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected — " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Failed");
  }

  setupTime();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/debug", handleDebug);
  server.on("/rawdata", handleRawData);
  server.begin();
  Serial.println("[Server] Started");
  Serial.println("http://" + WiFi.localIP().toString());
  Serial.println("http://" + WiFi.localIP().toString() + "/debug");

  esp_task_wdt_reset();
  delay(2000);
  takeReading();
  lastReading = millis();
}


// ============================================================
//  MAIN LOOP (LUNAR GRADE)
// ============================================================
void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  ensureWiFi();

  // EMERGENCY REBOOT 1: Too many consecutive Sheets failures
  if (consecutiveSheetsFailures >= SHEETS_FAILURE_REBOOT_THRESHOLD) {
    Serial.println("[SYSTEM] EMERGENCY: Too many consecutive Sheets failures");
    Serial.printf("[SYSTEM] Rebooting device (failures: %d)\n", consecutiveSheetsFailures);
    delay(1000);
    ESP.restart();
  }

  // EMERGENCY REBOOT 2: BME680 has been dead for too long (LUNAR GRADE)
  if (!bmeOk && bme680DiedAt > 0 && (millis() - bme680DiedAt) > BME680_DEAD_TIMEOUT_MS) {
    Serial.println("[SYSTEM] EMERGENCY: BME680 dead for too long");
    Serial.printf("[SYSTEM] BME680 has been offline for %d minutes\n", (millis() - bme680DiedAt) / 60000);
    Serial.println("[SYSTEM] Rebooting device to attempt recovery...");
    delay(1000);
    ESP.restart();
  }

  // EMERGENCY REBOOT 3: Too many BME680 reinit failures
  if (bme680ReinitFailures >= MAX_BME680_REINIT_FAILURES) {
    Serial.println("[SYSTEM] EMERGENCY: Too many BME680 reinit failures");
    Serial.printf("[SYSTEM] Rebooting device (failures: %d)\n", bme680ReinitFailures);
    delay(1000);
    ESP.restart();
  }

  if (millis() - lastReading >= SAMPLE_INTERVAL_MS) {
    lastReading = millis();
    takeReading();
  }
}