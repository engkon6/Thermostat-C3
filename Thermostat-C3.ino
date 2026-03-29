#include <LittleFS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <esp_sleep.h>

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
// GLOBALS & PIN DEFINITIONS
// ==========================================
#define BAT_SENSE_PIN 3  // GPIO 3 (A3) linked via solder jumper

WebServer httpServer(80);
HTTPUpdateServer httpUpdater;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

const int btnPins[4] = {2, 4, 5, 6}; 
bool btnState[4] = {HIGH, HIGH, HIGH, HIGH};

float batteryVoltage = 0.0;
int batteryPercentage = 0;

// Internal logic
bool childLockEnabled = true;
bool childUnlocked = false;
bool displayPower = true;
bool isDimmed = false;
bool display_flip = false;
bool tempValid = true;
bool mqttConnected = false;
bool unlockComboActive = false;

unsigned long lastUserActivity = 0;
unsigned long unlockStartTime = 0;
unsigned long unlockTime = 0;
unsigned long lastMqttAttempt = 0;
unsigned long btnPressStart[4] = {0,0,0,0};
bool longPressHandled[4] = {false,false,false,false};

const unsigned long longPressTime = 1000;
const unsigned long unlockHoldTime = 2000;
const unsigned long unlockTimeout = 10000;
const unsigned long dimTimeout = 30000;
const unsigned long sleepTimeout = 300000;

char mqtt_server[40] = "192.168.2.10";
char mqtt_port[6] = "1883";
char mqtt_user[20] = "";
char mqtt_pass[20] = "";
char topic_sub[50] = "stat/tasmota_181EC4/temperature";
char topic_pub[50] = "cmnd/tasmota_181EC4/mem1";
char topic_set_sub[50] = "stat/tasmota_181EC4/setpoint";
char topic_stat[50] = "stat/tasmota_181EC4/POWER";
char device_hostname[32] = {0};

double Temp = 0.0;
double Temp2 = 0.0;
int deviceStatus = 0;
time_t lastTempReceived = 0;

const int fullContrast = 255;
const int dimContrast = 30;
bool sleepEnabled = true;

// ==========================================
// ACCURATE BATTERY LOGIC
// ==========================================
void updateBattery() {
  uint32_t total_mV = 0;
  const int samples = 15;
  
  for(int i = 0; i < samples; i++) {
    total_mV += analogReadMilliVolts(BAT_SENSE_PIN);
    delay(1);
  }
  uint32_t avg_pin_mV = total_mV / samples;
  batteryVoltage = (avg_pin_mV * 2.0) / 1000.0;

  // Improved mapping (3.5V to 4.2V range)
  float pct = (batteryVoltage - 3.5) / (4.2 - 3.5) * 100.0;
  batteryPercentage = constrain((int)pct, 0, 100);
}

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
    v = json["mqtt_server"] | mqtt_server; strncpy(mqtt_server, v, 39);
    v = json["mqtt_port"] | mqtt_port; strncpy(mqtt_port, v, 5);
    v = json["mqtt_user"] | mqtt_user; strncpy(mqtt_user, v, 19);
    v = json["mqtt_pass"] | mqtt_pass; strncpy(mqtt_pass, v, 19);
    v = json["topic_sub"] | topic_sub; strncpy(topic_sub, v, 49);
    v = json["topic_pub"] | topic_pub; strncpy(topic_pub, v, 49);
    v = json["topic_set_sub"] | topic_set_sub; strncpy(topic_set_sub, v, 49);
    v = json["topic_stat"] | topic_stat; strncpy(topic_stat, v, 49);
    v = json["hostname"] | device_hostname; strncpy(device_hostname, v, 31);
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
  if (configFile) { serializeJson(json, configFile); configFile.close(); }
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
.statusbar{ display:flex; justify-content:space-between; font-size:14px; margin-bottom:8px; }
.ok{color:#0f0;} .bad{color:#f33;}
</style>
</head>
<body>
<div class="card">
<h2 id="hostname_disp"></h2>
<div class="statusbar">
  <span id="wifi">WiFi</span>
  <span id="time">--:--</span>
  <span id="mqtt">MQTT</span>
  <span id="bat">--%</span>
</div>
<div class="temp"><span id="curr_temp">--</span>&deg;C</div>
<p>Relay: <span id="relay_stat" class="status-badge">--</span></p>
<hr>
<button onclick="saveConfig()" style="background:#f1c40f">Save & Restart</button>
</div>
<script>
async function updateStatus(){
  try{
    const r = await fetch("/api/status");
    const s = await r.json();
    document.getElementById("curr_temp").innerText = s.temp.toFixed(1);
    document.getElementById("bat").innerText = s.bat + "%";
    document.getElementById("relay_stat").innerText = s.relay == 1 ? "ON" : "OFF";
    document.getElementById("relay_stat").className = s.relay == 1 ? "status-badge on" : "status-badge off";
    document.getElementById("mqtt").className = s.mqtt ? "ok" : "bad";
  }catch(e){}
}
setInterval(updateStatus,2000);
updateStatus();
</script>
</body>
</html>
)====";

// ==========================================
// MQTT & BUTTON HANDLERS
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';
  if (strcmp(topic, topic_sub) == 0) { Temp = atof(msg); time(&lastTempReceived); }
  else if (strcmp(topic, topic_stat) == 0) {
    deviceStatus = (!strcasecmp(msg, "1") || !strcasecmp(msg, "ON")) ? 1 : 0;
  }
  else if (strcmp(topic, topic_set_sub) == 0) { Temp2 = atof(msg); }
}

void turnDisplayOn() {
  if (!displayPower) { display.displayOn(); display.setContrast(fullContrast); displayPower = true; isDimmed = false; lastUserActivity = millis(); }
}
void turnDisplayOff() { display.displayOff(); display.clear(); display.display(); displayPower = false; }

void mqttReconnect() {
  static uint8_t retryCount = 0;
  if (mqttClient.connected()) { retryCount = 0; mqttConnected = true; return; }
  unsigned long dMs = min(30000UL, 5000UL * (1 << retryCount));
  if (millis() - lastMqttAttempt < dMs) return;
  lastMqttAttempt = millis();
  if (mqttClient.connect(device_hostname, mqtt_user, mqtt_pass)) {
    mqttClient.subscribe(topic_sub); mqttClient.subscribe(topic_stat); mqttClient.subscribe(topic_set_sub);
    mqttConnected = true; lastUserActivity = millis(); retryCount = 0;
  } else { mqttConnected = false; if (retryCount < 5) retryCount++; }
}

// ==========================================
// UI & OLED DRAWING
// ==========================================
void drawWifiBars(int x, int y) {
  int rssi = WiFi.RSSI();
  int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int barH = (i + 1) * 3;
    if (i < bars) display.fillRect(x + (i * 4), y - barH, 3, barH);
    else display.drawRect(x + (i * 4), y - barH, 3, barH);
  }
}

void TH_Overlay() {
  display.clear();
  updateBattery();

  static bool blinkState = false;
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > (mqttConnected ? 1000 : 250)) { lastBlink = millis(); blinkState = !blinkState; }

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if (mqttConnected) drawWifiBars(2, 14);
  else if (blinkState) display.drawString(2, 2, "MQTT !");

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W / 2, 2, deviceStatus == 1 ? "ON" : "OFF");

  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(SCREEN_W - 2, 2, String(batteryPercentage) + "%");

  if (lastTempReceived > 0) {
    struct tm * ti = localtime(&lastTempReceived);
    char buf[6]; strftime(buf, 6, "%H:%M", ti);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(2, SCREEN_H - 12, String(buf));
  }

  display.setFont(ArialMT_Plain_24);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W / 2, 20, tempValid ? String(Temp, 1) + "°C" : "--.-°C");
    
  display.setFont(ArialMT_Plain_16);
  static bool setBlinkState = true;
  static unsigned long setLastBlink = 0;
  if (childUnlocked) { if (millis() - setLastBlink > 500) { setBlinkState = !setBlinkState; setLastBlink = millis(); } }
  else { setBlinkState = true; }

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W / 2 - 30, SCREEN_H - 18, "Set:");
  if (setBlinkState) display.drawString(SCREEN_W / 2 + 10, SCREEN_H - 18, String(Temp2,1) + "°C");
  display.display();
}

void handleButtons() {
  bool b0 = (digitalRead(btnPins[0]) == LOW);
  bool b1 = (digitalRead(btnPins[1]) == LOW);
  if (childLockEnabled && !childUnlocked) {
    if (b0 && b1) {
      if (!unlockComboActive) { unlockComboActive = true; unlockStartTime = millis(); }
      if (millis() - unlockStartTime >= unlockHoldTime) { childUnlocked = true; unlockTime = millis(); unlockComboActive = false; lastUserActivity = millis(); if (isDimmed) { display.setContrast(fullContrast); isDimmed = false; } }
    } else { unlockComboActive = false; }
  }
  for (int i = 0; i < 4; i++) {
    int rd = digitalRead(btnPins[i]);
    if (rd == LOW && btnState[i] == HIGH) {
      if (!displayPower && i != 3) turnDisplayOn();
      btnPressStart[i] = millis(); longPressHandled[i] = false; lastUserActivity = millis();
      if (isDimmed) { display.setContrast(fullContrast); isDimmed = false; }
    }
    if (i == 2 && rd == LOW && !longPressHandled[i]) {
      if (millis() - btnPressStart[i] >= longPressTime) {
        display_flip = !display_flip;
        if (display_flip) display.flipScreenVertically(); else display.resetOrientation();
        saveConfig(); longPressHandled[i] = true;
      }
    }
    if (rd == HIGH && btnState[i] == LOW) {
      if (i == 3) { if (displayPower) turnDisplayOff(); else turnDisplayOn(); }
      if ((i == 0 || i == 1) && childUnlocked) {
        Temp2 += (i == 0) ? (display_flip ? 0.5 : -0.5) : (display_flip ? -0.5 : 0.5);
        Temp2 = constrain(Temp2, 10.0, 35.0);
        mqttClient.publish(topic_pub, String(Temp2).c_str(), true);
        unlockTime = millis();
      }
    }
    btnState[i] = rd;
  }
  if (childLockEnabled && childUnlocked && millis() - unlockTime > unlockTimeout) childUnlocked = false;
}

void configModeCallback(WiFiManager *mw) {
  display.clear(); updateBattery();
  display.setTextAlignment(TEXT_ALIGN_RIGHT); display.setFont(ArialMT_Plain_10);
  display.drawString(SCREEN_W - 2, 2, String(batteryPercentage) + "%");
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W/2, 5, "WiFi Setup Mode");
  display.drawString(SCREEN_W/2, 20, "Connect to:");
  display.drawString(SCREEN_W/2, 32, mw->getConfigPortalSSID());
  display.drawString(SCREEN_W/2, 48, "192.168.4.1");
  display.display();
}

void goToDeepSleep() {
  display.clear(); display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(SCREEN_W/2, SCREEN_H/2 - 5, "Sleeping..."); display.display(); delay(500);
  uint64_t mask = (1ULL<<btnPins[0]) | (1ULL<<btnPins[1]) | (1ULL<<btnPins[2]) | (1ULL<<btnPins[3]);
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW); esp_deep_sleep_start();
}

void setupWebServer() {
  httpServer.on("/", []() { httpServer.send(200, "text/html", DASHBOARD_HTML); });
  httpServer.on("/api/status", []() {
    DynamicJsonDocument d(1024);
    d["temp"] = Temp; d["setpoint"] = Temp2; d["relay"] = deviceStatus;
    d["bat"] = batteryPercentage; d["mqtt"] = mqttConnected;
    String j; serializeJson(d, j); httpServer.send(200, "application/json", j);
  });
  httpUpdater.setup(&httpServer); httpServer.begin();
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  if (!LittleFS.begin(true)) Serial.println("FS Mount Failed");
  loadConfig();
  if (strlen(device_hostname) == 0) {
    uint64_t chipid = ESP.getEfuseMac(); sprintf(device_hostname, "C3-Pico-%06X", (uint32_t)(chipid & 0xFFFFFF));
  }
  display.init(); if(display_flip) display.flipScreenVertically(); else display.resetOrientation();
  display.setContrast(fullContrast); lastUserActivity = millis();
  for (int i=0;i<4;i++) pinMode(btnPins[i], INPUT_PULLUP);
  updateBattery();
  WiFiManager wm; wm.setAPCallback(configModeCallback);
  WiFi.hostname(device_hostname); WiFi.setTxPower(WIFI_POWER_8_5dBm);
  wm.autoConnect(device_hostname);
  configTime(8 * 3600, 0, "pool.ntp.org");
  mqttClient.setServer(mqtt_server, atoi(mqtt_port)); mqttClient.setCallback(mqttCallback);
  setupWebServer();
}

void loop() {
  httpServer.handleClient(); mqttReconnect(); mqttClient.loop(); handleButtons();
  if (displayPower) { static unsigned long lastDraw = 0; if (millis() - lastDraw > 1000) { lastDraw = millis(); TH_Overlay(); } }
  if (displayPower && !isDimmed && millis() - lastUserActivity > dimTimeout) { display.setContrast(dimContrast); isDimmed = true; }
  tempValid = (lastTempReceived > 0 && (time(nullptr) - lastTempReceived < 300));
  if (sleepEnabled && !displayPower && (millis() - lastUserActivity > sleepTimeout)) goToDeepSleep();
}
