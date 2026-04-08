#include <LittleFS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <SPIFFS.h>   // ESP32 uses SPIFFS or LittleFS (ESP32 version)
#include <esp_sleep.h>

// Function prototypes
void turnDisplayOn();
void turnDisplayOff();
void goToDeepSleep();

// ==========================================
// DISPLAY CONFIGURATION
// ==========================================
#define USE_SH1106_128x64
#ifdef USE_SH1106_128x64
  #include "SH1106Wire.h"
  SH1106Wire display(0x3c, 8, 10);
  #define SCREEN_W 128
  #define SCREEN_H 64
#else
  #include "SSD1306Wire.h"
  SSD1306Wire display(0x3c, 4, 5);
  #define SCREEN_W 64
  #define SCREEN_H 48
#endif

// ==========================================
// GLOBALS
// ==========================================
WebServer httpServer(80);
HTTPUpdateServer httpUpdater;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// CHANGED: Button 2 moved to 4 to free 3 for battery
const int btnPins[4] = {2, 4, 5, 6}; 
bool btnState[4] = {HIGH, HIGH, HIGH, HIGH};

float batteryVoltage = 0.0;
int batteryPercentage = 0;

// ===== LONG PRESS CONFIG =====
const unsigned long longPressTime = 1000;   // 1 second
unsigned long btnPressStart[4] = {0,0,0,0};
bool longPressHandled[4] = {false,false,false,false};

// ===== CHILD LOCK (Dual Button) =====
bool childLockEnabled = true;
bool childUnlocked = false;

bool displayPower = true;     // true = display ON

unsigned long unlockStartTime = 0;
bool unlockComboActive = false;

const unsigned long unlockHoldTime = 2000;   // 2 seconds
const unsigned long unlockTimeout  = 10000;  // 10 sec auto lock
unsigned long unlockTime = 0;

bool mqttConnected = false;
unsigned long lastTempUpdate = 0;
float lastTemp = 0;
bool display_flip = false; // FEATURE 1: Flip state

char mqtt_server[40] = "YOUR_MQTT_BROKER_IP";
char mqtt_port[6] = "1883";
char mqtt_user[20] = "";
char mqtt_pass[20] = "";
char topic_sub[50] = "your/subscribe/topic";
char topic_pub[50] = "your/publish/topic";
char topic_set_sub[50] = "your/setpoint/topic";
char topic_stat[50] = "your/status/topic";
char device_hostname[32] = {0};   // will be generated dynamically

double Temp = 0.0;
double Temp2 = 0.0;
int deviceStatus = 0;

time_t lastTempReceived = 0;
unsigned long lastMqttAttempt = 0;

bool tempValid = true;

// ===== AUTO DIM =====
unsigned long lastUserActivity = 0;
const unsigned long dimTimeout = 30000;   // 30 sec
bool isDimmed = false;
const int fullContrast = 255;
const int dimContrast = 30;

// ===== DEEP SLEEP =====
const unsigned long sleepTimeout = 300000; // 5 minutes idle before sleep
bool sleepEnabled = true;

// ==========================================
// CONFIG STORAGE
// ==========================================
void loadConfig() {
  if (!LittleFS.exists("/config.json")) return;

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return;

  DynamicJsonDocument json(1024);
  if (deserializeJson(json, configFile) == DeserializationError::Ok) {

    const char* v;

    v = json["mqtt_server"] | mqtt_server;
    strncpy(mqtt_server, v, sizeof(mqtt_server) - 1);
    mqtt_server[sizeof(mqtt_server) - 1] = '\0';

    v = json["mqtt_port"] | mqtt_port;
    strncpy(mqtt_port, v, sizeof(mqtt_port) - 1);
    mqtt_port[sizeof(mqtt_port) - 1] = '\0';

    v = json["mqtt_user"] | mqtt_user;
    strncpy(mqtt_user, v, sizeof(mqtt_user) - 1);
    mqtt_user[sizeof(mqtt_user) - 1] = '\0';

    v = json["mqtt_pass"] | mqtt_pass;
    strncpy(mqtt_pass, v, sizeof(mqtt_pass) - 1);
    mqtt_pass[sizeof(mqtt_pass) - 1] = '\0';

    v = json["topic_sub"] | topic_sub;
    strncpy(topic_sub, v, sizeof(topic_sub) - 1);
    topic_sub[sizeof(topic_sub) - 1] = '\0';

    v = json["topic_pub"] | topic_pub;
    strncpy(topic_pub, v, sizeof(topic_pub) - 1);
    topic_pub[sizeof(topic_pub) - 1] = '\0';

    v = json["topic_set_sub"] | topic_set_sub;
    strncpy(topic_set_sub, v, sizeof(topic_set_sub) - 1);
    topic_set_sub[sizeof(topic_set_sub) - 1] = '\0';

    v = json["topic_stat"] | topic_stat;
    strncpy(topic_stat, v, sizeof(topic_stat) - 1);
    topic_stat[sizeof(topic_stat) - 1] = '\0';

    v = json["hostname"] | device_hostname;
    strncpy(device_hostname, v, sizeof(device_hostname) - 1);
    device_hostname[sizeof(device_hostname) - 1] = '\0';

    display_flip = json["flip"] | false;
  }

  configFile.close();
}

void saveConfig() {
  DynamicJsonDocument json(1024);
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_user"] = mqtt_user;
  json["mqtt_pass"] = mqtt_pass;
  json["topic_sub"] = topic_sub;
  json["topic_pub"] = topic_pub;
  json["topic_set_sub"] = topic_set_sub;
  json["topic_stat"] = topic_stat;
  json["hostname"] = device_hostname;
  json["flip"] = display_flip;

  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(json, configFile);
    configFile.close();
  }
}

// ==========================================
// MQTT & BUTTONS
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  char msg[64];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (strcmp(topic, topic_sub) == 0) {
    Temp = atof(msg);
    time(&lastTempReceived);
  }
  else if (strcmp(topic, topic_stat) == 0) {
    if (!strcasecmp(msg, "1") || !strcasecmp(msg, "ON") || !strcasecmp(msg, "TRUE"))
      deviceStatus = 1;
    else
      deviceStatus = 0;
  }
  else if (strcmp(topic, topic_set_sub) == 0) {
    Temp2 = atof(msg);
  }
}

void turnDisplayOn() {
  if (!displayPower) {
    display.displayOn();
    display.setContrast(fullContrast);
    displayPower = true;
    isDimmed = false;
    lastUserActivity = millis();
  }
}

void turnDisplayOff() {
  // If using the "ESP8266_and_ESP32_OLED_driver" (SH1106Wire)
  display.displayOff(); 
  
  // Optional: Clear the buffer so it's fresh when it wakes up
  display.clear();
  display.display();
}

void mqttReconnect() {
  static uint8_t retryCount = 0;

  if (mqttClient.connected()) {
    retryCount = 0;
    mqttConnected = true;
    return;
  }

  unsigned long delayMs = min(30000UL, 5000UL * (1 << retryCount));
  if (millis() - lastMqttAttempt < delayMs) return;

  lastMqttAttempt = millis();

  if (mqttClient.connect(device_hostname, mqtt_user, mqtt_pass)) {

    mqttClient.subscribe(topic_sub);
    mqttClient.subscribe(topic_stat);
    mqttClient.subscribe(topic_set_sub);

    mqttConnected = true;
    lastUserActivity = millis();
    retryCount = 0;

  } else {
    mqttConnected = false;
    if (retryCount < 5) retryCount++;
  }
}


// ==========================================
// DISPLAY
// ==========================================

void drawWifiBars(int x, int y) {
  int rssi = WiFi.RSSI();
  int bars = 0;

  if (rssi > -55) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  else bars = 0;

  for (int i = 0; i < 4; i++) {
    int barHeight = (i + 1) * 3;
    int barX = x + (i * 4);
    int barY = y - barHeight;

    if (i < bars)
      display.fillRect(barX, barY, 3, barHeight);
    else
      display.drawRect(barX, barY, 3, barHeight);
  }
}

void TH_Overlay() {
  display.clear();
  
  // Read battery status
  uint32_t mV = analogReadMilliVolts(3);
  batteryVoltage = (mV * 2.0) / 1000.0; // 1:2 divider
  // Mapping: 3.2V = 0%, 4.2V = 100%
  batteryPercentage = constrain(map(mV * 2, 3200, 4200, 0, 100), 0, 100);

  Serial.print("Battery: ");
  Serial.print(mV);
  Serial.print("mV (raw) | ");
  Serial.print(batteryVoltage);
  Serial.print("V | ");
  Serial.print(batteryPercentage);
  Serial.println("%");

  static bool blinkState = false;
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > (mqttConnected ? 1000 : 250)) {
    lastBlink = millis();
    blinkState = !blinkState;
  }

  display.setFont(ArialMT_Plain_10);

  // TOP LEFT - WiFi
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if (mqttClient.connected()) {
    drawWifiBars(2, 14);   // your existing WiFi icon
  } else {
    if (blinkState) {
      display.drawString(2, 2, "MQTT !");
    }
  }

  // TOP MIDDLE - Relay Status
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W / 2, 2, deviceStatus == 1 ? "ON" : "OFF");

  // TOP RIGHT - Battery Indicator
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(SCREEN_W - 2, 2, String(batteryPercentage) + "%");

  // BOTTOM LEFT - Time
  if (lastTempReceived > 0) {
    struct tm * timeinfo = localtime(&lastTempReceived);
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", timeinfo);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(2, SCREEN_H - 12, String(buf));
  }

  display.setFont(ArialMT_Plain_24);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  if(tempValid)
    display.drawString(SCREEN_W / 2, 20, String(Temp, 1) + "°C");
  else
    display.drawString(SCREEN_W / 2, 20, "--.-°C");
    
  display.setFont(ArialMT_Plain_16);

  // ===============================
  // SETPOINT BLINK (separate timer)
  // ===============================
  static bool setBlinkState = true;
  static unsigned long setLastBlink = 0;

  if (childUnlocked) {
    if (millis() - setLastBlink > 500) {
      setBlinkState = !setBlinkState;
      setLastBlink = millis();
    }
  } else {
    setBlinkState = true;  // always visible when locked
  }

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W / 2 - 30, SCREEN_H - 18, "Set:");

  if (setBlinkState)
    display.drawString(SCREEN_W / 2 + 10, SCREEN_H - 18, String(Temp2,1) + "°C");

  display.display();
}

void handleButtons() {
  bool btn0Pressed = (digitalRead(btnPins[0]) == LOW);
  bool btn1Pressed = (digitalRead(btnPins[1]) == LOW);

  // =========================================
  // 🔐 DUAL BUTTON HOLD TO UNLOCK
  // =========================================
  if (childLockEnabled && !childUnlocked) {
    if (btn0Pressed && btn1Pressed) {
      if (!unlockComboActive) {
        unlockComboActive = true;
        unlockStartTime = millis();
      }
      if (millis() - unlockStartTime >= unlockHoldTime) {
        childUnlocked = true;
        unlockTime = millis();
        unlockComboActive = false;
        lastUserActivity = millis();
        if (isDimmed) {
          display.setContrast(fullContrast);
          isDimmed = false;
        }
      }
    } else {
      unlockComboActive = false;
    }
  }

  // =========================================
  // BUTTON EDGE DETECTION
  // =========================================
  for (int i = 0; i < 4; i++) {
    int reading = digitalRead(btnPins[i]);

    // PRESS START
    if (reading == LOW && btnState[i] == HIGH) {
      // FIX: Only wake display if it's OFF AND the button pressed IS NOT the toggle button (index 3)
      // This prevents the toggle button from turning it ON then immediately OFF on release.
      if (!displayPower && i != 3) {
        turnDisplayOn();
      }
      
      btnPressStart[i] = millis();
      longPressHandled[i] = false;
      lastUserActivity = millis();
      
      if (isDimmed) {
        display.setContrast(fullContrast);
        isDimmed = false;
      }
    }

    // LONG PRESS (GPIO5 → Flip)
    if (i == 2 && reading == LOW && !longPressHandled[i]) {
      if (millis() - btnPressStart[i] >= longPressTime) {
        display_flip = !display_flip;
        if (display_flip) display.flipScreenVertically();
        else display.resetOrientation();
        saveConfig();
        longPressHandled[i] = true;
        lastUserActivity = millis();
      }
    }

    // RELEASE → Short Press Actions
    if (reading == HIGH && btnState[i] == LOW) {
      // =========================================
      // GPIO6 (Index 3) → Display Toggle
      // =========================================
      if (i == 3) {
        if (displayPower) {
          turnDisplayOff();
        } else {
          turnDisplayOn();
        }
      }

      // ===== Setpoint Buttons (GPIO2 & GPIO4 / Index 0 & 1) =====
      if ((i == 0 || i == 1) && childUnlocked) {
        lastUserActivity = millis();
        if (i == 0) {
          if (display_flip) Temp2 += 0.5;
          else Temp2 -= 0.5;
        }
        if (i == 1) {
          if (display_flip) Temp2 -= 0.5;
          else Temp2 += 0.5;
        }
        Temp2 = constrain(Temp2, 10.0, 35.0);
        mqttClient.publish(topic_pub, String(Temp2).c_str(), true);
        unlockTime = millis();
      }
    }
    btnState[i] = reading;
  }

  // AUTO RE-LOCK
  if (childLockEnabled && childUnlocked && millis() - unlockTime > unlockTimeout) {
    childUnlocked = false;
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  display.clear();
  
  // Read battery for display
  int raw = analogRead(3);
  float vbat = (raw * 3.3 / 4095.0) * 2.0;
  int pct = constrain(map(vbat * 100, 330, 420, 0, 100), 0, 100);

  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(SCREEN_W - 2, 2, String(pct) + "%");

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W/2, 5, "WiFi Setup Mode");
  display.drawString(SCREEN_W/2, 20, "Connect to:");
  display.drawString(SCREEN_W/2, 32, myWiFiManager->getConfigPortalSSID());
  display.drawString(SCREEN_W/2, 48, "192.168.4.1");

  display.display();

  Serial.print("Setup Mode Battery: ");
  Serial.print(raw);
  Serial.print(" (raw) | ");
  Serial.print(vbat);
  Serial.println("V");
}


void goToDeepSleep() {

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W/2, SCREEN_H/2 - 5, "Sleeping...");
  display.display();

  delay(500);

  // Wake when any button goes LOW
  uint64_t wakeMask =
      (1ULL << btnPins[0]) |
      (1ULL << btnPins[1]) |
      (1ULL << btnPins[2]) |
      (1ULL << btnPins[3]);

  esp_deep_sleep_enable_gpio_wakeup(wakeMask, ESP_GPIO_WAKEUP_GPIO_LOW);

  Serial.println("Entering deep sleep");

  delay(200);

  esp_deep_sleep_start();
}


// ==========================================
// DASHBOARD HTML
// ==========================================
const char* DASHBOARD_HTML = R"====(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: Arial; background:#1a1a1a; color:white; text-align:center; padding:10px; }
.card { background:#2d2d2d; padding:15px; border-radius:12px; max-width:420px; margin:auto; }
.temp { font-size:42px; color:#00d1b2; margin:10px 0; }
label { font-size:12px; color:#bbb; display:block; text-align:left; width:92%; margin:6px auto 2px auto; }
input { width:92%; padding:8px; margin-bottom:8px; border-radius:6px; border:none; background:#444; color:white; }
button { padding:10px 20px; border-radius:6px; border:none; background:#00d1b2; font-weight:bold; cursor:pointer; width:95%; }
.status-badge { padding: 4px 8px; border-radius: 4px; font-size: 12px; }
.on { background: #2ecc71; color: black; }
.off { background: #e74c3c; color: white; }

.header-container {
  display: flex;
  justify-content: center;
  align-items: center;
  gap: 10px;
  margin-bottom: 5px;
}

.set-control {
  display:flex;
  justify-content:center;
  align-items:center;
  gap:20px;
  margin:15px 0;
}

.set-value {
  font-size:32px;
  min-width:90px;
}

.btn-minus, .btn-plus {
  width:55px;
  height:55px;
  border-radius:50%;
  border:none;
  font-size:26px;
  font-weight:bold;
  cursor:pointer;
  background:#444;
  color:white;
}

.btn-minus:hover { background:#e74c3c; }
.btn-plus:hover { background:#2ecc71; }

.statusbar{
  display:flex;
  justify-content:space-between;
  font-size:14px;
  margin-bottom:8px;
}

.ok{color:#0f0;}
.bad{color:#f33;}
.warn{color:#ffa500;}

.wifi-bar {
  display:inline-block;
  width:4px;
  margin-right:2px;
  background:#444;
}

.wifi-on {
  background:#2ecc71;
}

.rotate-icon{
  cursor:pointer;
  font-size:20px;
  color:#bbb;
  transition:0.2s;
  line-height: 1;
}

.rotate-icon:hover{
  color:#00d1b2;
  transform:rotate(90deg);
}

</style>
</head>
<body>
<div class="card">
<div class="header-container">
  <h2 id="hostname_disp" style="margin: 10px 0;"></h2>
  <span id="rotate_btn" class="rotate-icon" onclick="toggleFlip()">⟳</span>
</div>

<div class="statusbar">
  <span id="wifi"></span>
  <span id="time">--:--</span>
  <span id="mqtt">MQTT</span>
  <span id="bat">--%</span>
</div>
<div class="temp"><span id="curr_temp">--</span>&deg;C</div>
<p>Relay: <span id="relay_stat" class="status-badge">--</span></p>

<p>Setpoint</p>

<div class="set-control">
  <button class="btn-minus" onclick="adjustSetpoint(-0.5)">−</button>
  <span id="set_temp_disp" class="set-value">--</span>
  <button class="btn-plus" onclick="adjustSetpoint(0.5)">+</button>
</div>

<hr>
<h3>Settings</h3>
<label>Hostname</label><input id="hostname_in">
<label>MQTT Server</label><input id="mqtt_server_in">
<label>Subscribe (Temp)</label><input id="topic_sub_in">
<label>Subscribe (Setpoint)</label><input id="topic_set_sub_in">
<label>Status (Relay)</label><input id="topic_stat_in">
<label>Publish (Setpoint)</label><input id="topic_pub_in">

<button onclick="saveConfig()" style="background:#f1c40f">Save & Restart</button>
</div>

<script>
// ... (rest of your JavaScript remains the same)
function updateData(){
 fetch('/api/status').then(r=>r.json()).then(data=>{
   document.getElementById('curr_temp').innerText = data.temp.toFixed(1);
   document.getElementById('set_temp_disp').innerText = data.setpoint.toFixed(1) + "°C";
   document.getElementById('hostname_disp').innerText = data.host;
   
   const rs = document.getElementById('relay_stat');
   rs.innerText = data.relay == 1 ? "ON" : "OFF";
   rs.className = data.relay == 1 ? "status-badge on" : "status-badge off";

   document.getElementById('hostname_in').value = data.host || "";
   document.getElementById('mqtt_server_in').value = data.mqtt_server;
   document.getElementById('topic_sub_in').value = data.topic_sub;
   document.getElementById('topic_set_sub_in').value = data.topic_set_sub;
   document.getElementById('topic_stat_in').value = data.topic_stat;
   document.getElementById('topic_pub_in').value = data.topic_pub;
 });
}

function adjustSetpoint(delta){
  let current = parseFloat(document.getElementById('set_temp_disp').innerText);
  if (isNaN(current)) return;

  let newValue = (current + delta).toFixed(1);

  fetch('/api/set', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'setpoint=' + newValue
  });
}

function updateWifiBars(rssi, connected){
  const wifi = document.getElementById("wifi");

  if(!connected){
    wifi.innerHTML = "WiFi ✕";
    wifi.className = "bad";
    return;
  }

  let bars = 0;
  if (rssi > -55) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  else bars = 0;

  let html = "";
  for(let i=0;i<4;i++){
    html += `<span class="wifi-bar ${i<bars?'wifi-on':''}" style="height:${(i+1)*4}px"></span>`;
  }

  wifi.innerHTML = html;
}

function toggleFlip(){
  fetch('/api/flip', { method:'POST' })
    .then(r=>r.text())
    .then(()=>{
      updateStatus();
    });
}

function saveConfig(){
 let params = new URLSearchParams();

 params.append('hostname', document.getElementById('hostname_in').value);
 params.append('mqtt_server', document.getElementById('mqtt_server_in').value);
 params.append('topic_sub', document.getElementById('topic_sub_in').value);
 params.append('topic_set_sub', document.getElementById('topic_set_sub_in').value);
 params.append('topic_stat', document.getElementById('topic_stat_in').value);
 params.append('topic_pub', document.getElementById('topic_pub_in').value);

 fetch('/api/config', { method:'POST', body: params })
   .then(r=>r.text())
   .then(alert);
}

updateData();

async function updateStatus(){
  try{
    const r = await fetch("/api/status");
    const s = await r.json();

    updateWifiBars(s.rssi, s.wifi);

    const mqtt = document.getElementById("mqtt");
    mqtt.className = s.mqtt ? "ok" : "bad";
    mqtt.innerText = "MQTT";

    document.getElementById("curr_temp").innerText = s.temp.toFixed(1);
    document.getElementById("set_temp_disp").innerText = s.setpoint.toFixed(1) + "°C";

    const rs = document.getElementById("relay_stat");
    rs.innerText = s.relay == 1 ? "ON" : "OFF";
    rs.className = s.relay == 1 ? "status-badge on" : "status-badge off";

    document.getElementById("time").innerText = s.last;
    document.getElementById("bat").innerText = s.bat + "%";

    document.getElementById("rotate_btn").style.color =
        s.flip ? "#00d1b2" : "#bbb";

  }catch(e){
    console.log("Status update error", e);
  }
}

setInterval(updateStatus,2000);
updateStatus();
</script>
</body>
</html>
)====";

// ==========================================
// WEB SERVER
// ==========================================
void setupWebServer() {
  httpServer.on("/", []() { httpServer.send(200, "text/html", DASHBOARD_HTML); });

  httpServer.on("/api/status", []() {
    DynamicJsonDocument doc(1024);
    doc["temp"] = Temp;
    doc["setpoint"] = Temp2;
    doc["relay"] = deviceStatus;
    doc["wifi"] = (WiFi.status() == WL_CONNECTED);
    doc["rssi"] = WiFi.RSSI();

    if (lastTempReceived > 0) {
      struct tm * timeinfo = localtime(&lastTempReceived);
      char buf[6];
      strftime(buf, sizeof(buf), "%H:%M", timeinfo);
      doc["last"] = buf;
    } else {
      doc["last"] = "--:--";
    }

    doc["mqtt"] = mqttConnected;
    doc["host"] = device_hostname;
    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_port"] = mqtt_port;
    doc["topic_sub"] = topic_sub;
    doc["topic_set_sub"] = topic_set_sub;
    doc["topic_pub"] = topic_pub;
    doc["topic_stat"] = topic_stat;
    doc["flip"] = display_flip;
    doc["bat"] = batteryPercentage;
    String json;
    serializeJson(doc, json);
    httpServer.send(200, "application/json", json);
  });

  httpServer.on("/api/set", HTTP_POST, []() {
    if (httpServer.hasArg("setpoint")) {
      Temp2 = constrain(httpServer.arg("setpoint").toFloat(), 10.0, 35.0);
      mqttClient.publish(topic_pub, String(Temp2).c_str(), true);
    }
    httpServer.send(200, "text/plain", "OK");
  });

  httpServer.on("/api/flip", HTTP_POST, [](){

    display_flip = !display_flip;

    if(display_flip)
      display.flipScreenVertically();
    else
      display.resetOrientation();

    saveConfig();
    
    httpServer.send(200, "text/plain", "OK");
  });

  httpServer.on("/api/config", HTTP_POST, []() {

    bool mqttNeedsReconnect = false;

    if (httpServer.hasArg("hostname")) {
      String v = httpServer.arg("hostname");
      strncpy(device_hostname, v.c_str(), sizeof(device_hostname) - 1);
      device_hostname[sizeof(device_hostname) - 1] = '\0';
    }

    if (httpServer.hasArg("mqtt_server")) {
      String v = httpServer.arg("mqtt_server");
      strncpy(mqtt_server, v.c_str(), sizeof(mqtt_server) - 1);
      mqtt_server[sizeof(mqtt_server) - 1] = '\0';
      mqttNeedsReconnect = true;
    }

    if (httpServer.hasArg("topic_sub")) {
      String v = httpServer.arg("topic_sub");
      strncpy(topic_sub, v.c_str(), sizeof(topic_sub) - 1);
      topic_sub[sizeof(topic_sub) - 1] = '\0';
      mqttNeedsReconnect = true;
    }

    if (httpServer.hasArg("topic_set_sub")) {
      String v = httpServer.arg("topic_set_sub");
      strncpy(topic_set_sub, v.c_str(), sizeof(topic_set_sub) - 1);
      topic_set_sub[sizeof(topic_set_sub) - 1] = '\0';
      mqttNeedsReconnect = true;
    }

    if (httpServer.hasArg("topic_pub")) {
      String v = httpServer.arg("topic_pub");
      strncpy(topic_pub, v.c_str(), sizeof(topic_pub) - 1);
      topic_pub[sizeof(topic_pub) - 1] = '\0';
    }

    if (httpServer.hasArg("topic_stat")) {
      String v = httpServer.arg("topic_stat");
      strncpy(topic_stat, v.c_str(), sizeof(topic_stat) - 1);
      topic_stat[sizeof(topic_stat) - 1] = '\0';
      mqttNeedsReconnect = true;
    }

    if (httpServer.hasArg("flip")) {
      display_flip = (httpServer.arg("flip") == "1");

      if(display_flip)
        display.flipScreenVertically();
      else
        display.resetOrientation();
    }

    saveConfig();

    // Apply MQTT changes live
    if (mqttNeedsReconnect) {
      mqttClient.disconnect();
      mqttClient.setServer(mqtt_server, atoi(mqtt_port));
      mqttReconnect();
    }

    httpServer.send(200, "text/plain", "Saved & Applied (No Restart)");
  });

  httpUpdater.setup(&httpServer);
  httpServer.begin();
}

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db);

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

  if (wakeReason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Wakeup caused by button");
  }

  if (!LittleFS.begin(true)) {
  Serial.println("LittleFS Mount Failed");
  }
  
  loadConfig();

  // If hostname not set from config, generate default
  if (strlen(device_hostname) == 0) {
    uint64_t chipid = ESP.getEfuseMac();
    uint32_t shortId = (uint32_t)(chipid & 0xFFFFFF);

    String defaultHost = "Thermostat-" + String(shortId, HEX);

    strncpy(device_hostname, defaultHost.c_str(), sizeof(device_hostname) - 1);
    device_hostname[sizeof(device_hostname) - 1] = '\0';
  }

  display.init();
  if(display_flip) display.flipScreenVertically();
  else display.resetOrientation();
  
  display.setContrast(fullContrast);
  lastUserActivity = millis();

  for (int i=0;i<4;i++) pinMode(btnPins[i], INPUT_PULLUP);

  // Initial battery check for verification
  int raw_bat = analogRead(3);
  float vbat_init = (raw_bat * 3.3 / 4095.0) * 2.0;
  Serial.print("Boot Battery: ");
  Serial.print(raw_bat);
  Serial.print(" (raw) | ");
  Serial.print(vbat_init);
  Serial.println("V");

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  WiFi.hostname(device_hostname);   // Apply immediately
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  wm.autoConnect(device_hostname);
  
  configTime(8 * 3600, 0, "pool.ntp.org");

  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);
  mqttClient.setServer(mqtt_server, atoi(mqtt_port));
  mqttClient.setCallback(mqttCallback);
  
  setupWebServer();
}

void loop() {
  httpServer.handleClient();
  mqttReconnect();
  mqttClient.loop();
  handleButtons();

  if (displayPower) {
    static unsigned long lastDraw = 0;
    if (millis() - lastDraw > 1000) {
      lastDraw = millis();
      TH_Overlay();
    }
  }
  
  if (displayPower && !isDimmed && millis() - lastUserActivity > dimTimeout) {
    display.setContrast(dimContrast);
    isDimmed = true;
  }

  if (lastTempReceived > 0 && (time(nullptr) - lastTempReceived > 300)) {
      tempValid = false;
  } else {
      tempValid = true;
  }

  if (sleepEnabled &&
      !displayPower &&
      (millis() - lastUserActivity > sleepTimeout)) {
  
    goToDeepSleep();
  }
  
}
