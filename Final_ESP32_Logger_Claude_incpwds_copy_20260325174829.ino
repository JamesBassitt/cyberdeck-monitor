// ============================================================
//  ESP32 CYBERDECK — Environmental Monitor
//  DHT22 sensor → local dashboard + Google Sheets logging
//  Requires: DHT sensor library (Adafruit), Chart.js (CDN)
// ============================================================

// -------------------- LIBRARIES --------------------
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h>

// -------------------- SECRETS --------------------
// Move these to secrets.h and add secrets.h to .gitignore
// before pushing to GitHub
#include "secrets.h"
// secrets.h should contain:
//   const char* ssid      = "your_wifi_name";
//   const char* password  = "your_wifi_password";
//   const char* scriptURL = "your_apps_script_url";

// -------------------- SENSOR --------------------
#define DHTPIN    4
#define DHTTYPE   DHT22
DHT dht(DHTPIN, DHTTYPE);

// -------------------- SAMPLING --------------------
// How often to take and log a reading
#define SAMPLE_INTERVAL_MS 300000    // 30 seconds (debug)
// #define SAMPLE_INTERVAL_MS 300000  // 5 minutes (normal)
// #define SAMPLE_INTERVAL_MS 900000  // 15 minutes (low power)

// -------------------- DATA BUFFER --------------------
// Circular buffer — oldest reading is overwritten when full
// 240 readings at 30s = 2 hours of local history
#define MAX_LOG 240

float         tempLog[MAX_LOG];
float         humLog[MAX_LOG];
unsigned long timeLog[MAX_LOG];   // Unix epoch timestamps

int logIndex = 0;   // next write position
int logCount = 0;   // total readings stored (capped at MAX_LOG)

// -------------------- STATE --------------------
// Last known good readings — used by dashboard to avoid
// triggering a fresh sensor read on every HTTP request
float lastTemp = NAN;
float lastHum  = NAN;

// -------------------- TIMING --------------------
unsigned long lastReading = 0;

// -------------------- WEB SERVER --------------------
WebServer server(80);


// ============================================================
//  WIFI
// ============================================================

// Reconnect if WiFi drops — called every loop() with backoff
// so we don't hammer the WiFi stack on repeated failures
void ensureWiFi() {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastAttempt > 10000) {
    Serial.println("[WiFi] Lost connection, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    lastAttempt = millis();
  }
}


// ============================================================
//  NTP TIME SYNC
// ============================================================

// Sync the ESP32 system clock via NTP before we start logging.
// All timestamps in the buffer and in Google Sheets use this.
// Adjust the UTC offset if you want local time instead of UTC.
void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[Time] Syncing NTP");

  time_t now = time(nullptr);
  while (now < 100000) {     // wait for a valid timestamp
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }

  Serial.println("\n[Time] Synced — " + String(ctime(&now)));
}


// ============================================================
//  SENSOR READS (with retry)
// ============================================================

// DHT22 occasionally returns NaN on a single read.
// Retry up to 3 times with a short delay before giving up.

float readTemperatureSafe() {
  for (int i = 0; i < 3; i++) {
    float t = dht.readTemperature();
    if (!isnan(t)) return t;
    delay(100);
  }
  Serial.println("[Sensor] Temperature read failed after 3 attempts");
  return NAN;
}

float readHumiditySafe() {
  for (int i = 0; i < 3; i++) {
    float h = dht.readHumidity();
    if (!isnan(h)) return h;
    delay(100);
  }
  Serial.println("[Sensor] Humidity read failed after 3 attempts");
  return NAN;
}


// ============================================================
//  GOOGLE SHEETS LOGGING
// ============================================================

// Sends a single reading to the Apps Script web app via HTTP GET.
// The Apps Script appends a row to the Google Sheet.
// Timeout is kept short (3s) to avoid blocking the main loop.
void sendToSheets(float temp, float hum) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Sheets] Skipping — no WiFi");
    return;
  }

  HTTPClient http;
  String url = String(scriptURL)
             + "?device=1"
             + "&temp=" + String(temp, 1)
             + "&hum="  + String(hum,  1);

  http.begin(url);
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();

  if (code == 200) {
    Serial.println("[Sheets] OK");
  } else {
    Serial.printf("[Sheets] Failed — HTTP %d\n", code);
  }

  http.end();
}


// ============================================================
//  TAKE A READING
// ============================================================

// Called on startup and then every SAMPLE_INTERVAL_MS.
// Reads the sensor, stores in circular buffer, updates last
// known values, and sends to Google Sheets.
void takeReading() {
  float t = readTemperatureSafe();
  float h = readHumiditySafe();

  if (!isnan(t) && !isnan(h)) {
    // Store in circular buffer
    tempLog[logIndex] = t;
    humLog[logIndex]  = h;
    timeLog[logIndex] = time(nullptr);

    logIndex = (logIndex + 1) % MAX_LOG;
    if (logCount < MAX_LOG) logCount++;

    // Cache last known good values for dashboard
    lastTemp = t;
    lastHum  = h;

    Serial.printf("[Reading] Temp: %.1f C  Hum: %.1f%%\n", t, h);

    // Push to Google Sheets
    sendToSheets(t, h);

  } else {
    Serial.println("[Reading] Skipped — bad sensor data");
  }
}


// ============================================================
//  HTTP: /data  — JSON endpoint for the dashboard
// ============================================================

// Returns current readings + full log buffer as JSON.
// Uses cached lastTemp/lastHum to avoid triggering a fresh
// sensor read on every dashboard poll.
void handleData() {

  // Build status flags for the dashboard UI
  bool sensorOk = !isnan(lastTemp) && !isnan(lastHum);
  bool wifiOk   = (WiFi.status() == WL_CONNECTED);

  String json = "{";
  json += "\"temp\":"      + (sensorOk ? String(lastTemp, 1) : String("null")) + ",";
  json += "\"hum\":"       + (sensorOk ? String(lastHum,  1) : String("null")) + ",";
  json += "\"sensorOk\": ";
  json += sensorOk ? "true" : "false";
  json += ",";
  json += "\"wifiOk\": ";
  json += wifiOk ? "true" : "false";
  json += ",";

  // Time labels (Unix epoch — formatted in JS)
  json += "\"labels\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(timeLog[idx]);
    if (i < logCount - 1) json += ",";
  }

  // Temperature history
  json += "],\"temps\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(tempLog[idx], 1);
    if (i < logCount - 1) json += ",";
  }

  // Humidity history
  json += "],\"hums\":[";
  for (int i = 0; i < logCount; i++) {
    int idx = (logIndex - logCount + i + MAX_LOG) % MAX_LOG;
    json += String(humLog[idx], 1);
    if (i < logCount - 1) json += ",";
  }

  json += "]}";

  server.send(200, "application/json", json);
}


// ============================================================
//  HTTP: /  — Cyberdeck dashboard page
// ============================================================

void handleRoot() {
  String intervalText = String(SAMPLE_INTERVAL_MS / 1000);

  String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 CYBERDECK</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<style>

/* ---------- base ---------- */
* { box-sizing: border-box; }
body {
  background: #0a0f0a;
  color: #00ff9c;
  font-family: "Courier New", monospace;
  margin: 0;
  padding: 20px;
  text-shadow: 0 0 6px rgba(0,255,150,0.5);
}

/* ---------- headings ---------- */
h1 {
  text-align: center;
  font-size: 1.8em;
  letter-spacing: 3px;
  margin-bottom: 4px;
}
.subtitle {
  text-align: center;
  font-size: 0.7em;
  opacity: 0.5;
  letter-spacing: 2px;
  margin-bottom: 20px;
}

/* ---------- panels ---------- */
.panel {
  border: 1px solid #00ff9c;
  border-radius: 8px;
  padding: 20px;
  margin: 12px auto;
  max-width: 900px;
  background: rgba(0,20,10,0.7);
  box-shadow: 0 0 16px rgba(0,255,150,0.15);
}
.panel-title {
  font-size: 0.65em;
  letter-spacing: 2px;
  opacity: 0.5;
  margin-bottom: 14px;
}

/* ---------- metric cards ---------- */
.grid {
  display: flex;
  justify-content: space-around;
  flex-wrap: wrap;
  gap: 10px;
}
.card {
  text-align: center;
  min-width: 120px;
}
.value {
  font-size: 3em;
  font-weight: bold;
  line-height: 1;
}
.unit {
  font-size: 0.4em;
  opacity: 0.6;
  margin-left: 2px;
}
.label {
  font-size: 0.65em;
  letter-spacing: 2px;
  opacity: 0.6;
  margin-top: 6px;
}

/* ---------- status bar ---------- */
.status-bar {
  display: flex;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 8px;
  font-size: 0.7em;
  letter-spacing: 1px;
  margin-top: 16px;
  padding-top: 12px;
  border-top: 1px solid rgba(0,255,150,0.2);
}
.status-item {
  display: flex;
  align-items: center;
  gap: 6px;
}
.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #00ff9c;
  flex-shrink: 0;
  transition: background 0.4s;
  animation: pulse 1.5s infinite;
}
.dot.fault {
  background: #ff4444;
  animation: none;
}
@keyframes pulse {
  0%,100% { opacity: 1; }
  50%      { opacity: 0.25; }
}

/* ---------- chart ---------- */
canvas { max-height: 280px; }

/* ---------- footer ---------- */
footer {
  text-align: center;
  font-size: 0.65em;
  letter-spacing: 1px;
  opacity: 0.4;
  margin-top: 16px;
}

</style>
</head>
<body>

<h1>[ENV] CYBERDECK MONITOR</h1>
<div class="subtitle">ESP32 // DHT22 // v1.1</div>

<!-- Live readings panel -->
<div class="panel">
  <div class="panel-title">// LIVE READINGS</div>
  <div class="grid">

    <div class="card">
      <div class="value" id="temp">--<span class="unit">°C</span></div>
      <div class="label">TEMPERATURE</div>
    </div>

    <div class="card">
      <div class="value" id="hum">--<span class="unit">%</span></div>
      <div class="label">HUMIDITY</div>
    </div>

    <div class="card">
      <div class="value" id="clock" style="font-size:1.8em">--:--:--</div>
      <div class="label">SYSTEM TIME</div>
    </div>

  </div>

  <!-- Status indicators — updated reactively by JS -->
  <div class="status-bar">
    <div class="status-item">
      <div class="dot" id="sensor-dot"></div>
      <span id="sensor-status">SENSOR CHECKING...</span>
    </div>
    <div class="status-item">
      <div class="dot" id="wifi-dot"></div>
      <span id="wifi-status">WIFI CHECKING...</span>
    </div>
    <div class="status-item">
      <div class="dot"></div>
      <span id="last-update">AWAITING DATA</span>
    </div>
  </div>
</div>

<!-- Chart panel -->
<div class="panel">
  <div class="panel-title">// SENSOR LOG — LAST <span id="log-count">0</span> READINGS</div>
  <canvas id="chart"></canvas>
</div>

<footer>
  INTERVAL: )rawhtml" + intervalText + R"rawhtml(s &nbsp;|&nbsp; BUFFER: )rawhtml" + String(MAX_LOG) + R"rawhtml( READINGS &nbsp;|&nbsp; SHEETS: ACTIVE
</footer>

<script>

// ---------- Chart setup ----------
const ctx = document.getElementById('chart').getContext('2d');
const chart = new Chart(ctx, {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      {
        label: 'TEMP °C',
        data: [],
        borderColor: '#00ff9c',
        backgroundColor: 'rgba(0,255,150,0.05)',
        tension: 0.3,
        pointRadius: 2,
        yAxisID: 'y1'   // left axis
      },
      {
        label: 'HUM %',
        data: [],
        borderColor: '#00bfff',
        backgroundColor: 'rgba(0,191,255,0.05)',
        tension: 0.3,
        pointRadius: 2,
        yAxisID: 'y2'   // right axis — prevents scale clash
      }
    ]
  },
  options: {
    responsive: true,
    animation: false,
    interaction: { mode: 'index', intersect: false },
    scales: {
      x: {
        ticks: { color: '#00ff9c', font: { size: 10 }, maxTicksLimit: 8 },
        grid:  { color: 'rgba(0,255,150,0.08)' }
      },
      y1: {
        position: 'left',
        ticks: { color: '#00ff9c', font: { size: 10 } },
        grid:  { color: 'rgba(0,255,150,0.08)' },
        title: { display: true, text: '°C', color: '#00ff9c', font: { size: 10 } }
      },
      y2: {
        position: 'right',
        ticks: { color: '#00bfff', font: { size: 10 } },
        grid:  { drawOnChartArea: false },   // no double gridlines
        title: { display: true, text: '%',  color: '#00bfff', font: { size: 10 } }
      }
    },
    plugins: {
      legend: {
        labels: { color: '#00ff9c', font: { size: 11 }, boxWidth: 12 }
      }
    }
  }
});

// ---------- Helpers ----------

// Format a Unix epoch (seconds) into a readable time string
function formatTime(epoch) {
  return new Date(epoch * 1000).toLocaleTimeString();
}

// Update the status dot and label reactively
function setStatus(dotId, labelId, ok, okText, faultText) {
  const dot   = document.getElementById(dotId);
  const label = document.getElementById(labelId);
  dot.classList.toggle('fault', !ok);
  label.textContent = ok ? okText : faultText;
}

// ---------- Live clock (ticks every second, independent of data fetch) ----------
function updateClock() {
  document.getElementById('clock').textContent =
    new Date().toLocaleTimeString();
}

// ---------- Data fetch (polls /data every SAMPLE_INTERVAL_MS) ----------
async function update() {
  try {
    const r = await fetch('/data');
    const d = await r.json();

    // Update metric cards — guard against null (sensor fault)
    if (d.temp !== null) document.getElementById('temp').innerHTML = d.temp.toFixed(1) + '<span class="unit">°C</span>';
    if (d.hum  !== null) document.getElementById('hum').innerHTML  = d.hum.toFixed(1)  + '<span class="unit">%</span>';

    // Reactive status indicators
    setStatus('sensor-dot', 'sensor-status', d.sensorOk, 'SENSOR ONLINE',  'SENSOR FAULT');
    setStatus('wifi-dot',   'wifi-status',   d.wifiOk,   'WIFI LINK ACTIVE', 'WIFI FAULT');

    // Last updated timestamp
    document.getElementById('last-update').textContent =
      'UPDATED ' + new Date().toLocaleTimeString();

    // Reading count
    document.getElementById('log-count').textContent = d.labels.length;

    // Update chart
    chart.data.labels           = d.labels.map(formatTime);
    chart.data.datasets[0].data = d.temps;
    chart.data.datasets[1].data = d.hums;
    chart.update();

  } catch(e) {
    // Log fetch errors to console rather than silently swallowing them
    console.warn('[CYBERDECK] Fetch failed:', e);

    // Show fault state on status bar
    setStatus('sensor-dot', 'sensor-status', false, '', 'COMMS LOST');
    setStatus('wifi-dot',   'wifi-status',   false, '', 'COMMS LOST');
  }
}

// ---------- Start ----------
update();
updateClock();
setInterval(update,      30000);   // poll data every 30s
setInterval(updateClock, 1000);    // tick clock every 1s

</script>
</body>
</html>
)rawhtml";

  server.send(200, "text/html", html);
}


// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  dht.begin();

  // Connect to WiFi
  Serial.print("[WiFi] Connecting");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected — " + WiFi.localIP().toString());

  // Sync clock via NTP before first reading
  setupTime();

  // Register HTTP routes
  server.on("/",     handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("[Server] Started");

  // Wait for DHT22 to stabilise then take first reading
  delay(2000);
  takeReading();
}


// ============================================================
//  MAIN LOOP
// ============================================================

void loop() {
  // Handle any incoming HTTP requests
  server.handleClient();

  // Keep WiFi alive (reconnects with 10s backoff if dropped)
  ensureWiFi();

  // Take a reading on the configured interval
  if (millis() - lastReading >= SAMPLE_INTERVAL_MS) {
    takeReading();
    lastReading = millis();
  }
}