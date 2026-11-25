/**
 * Wi-Fi configuration portal + web dashboard.
 * - On boot: tries the saved network. If it connects, serves the dashboard.
 * - If not (no saved network, or it is unreachable): raises "ESP32-Setup"
 *   and serves the same page, whose Wi-Fi section lets the user configure it.
 * - After a successful connection the page shows the assigned IP + SSID and
 *   the setup access point is switched off.
 *
 * Sensor values are read from the globals defined in main.cpp.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <cstring>
#include <cmath>

#include "web_portal.h"
#include "web_page.h"

// ---------------------------------------------------------------------------
// Sensor state owned by main.cpp
// ---------------------------------------------------------------------------
extern int   micLevel;
extern float dhtTemperature;
extern float dhtHumidity;
extern bool  dhtValid;
extern float gasVoltage;
extern float gasRsRo;
extern const char* airQualityLabel(float rsRoRatio);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const char* AP_SSID = "ESP32-Setup";
static const char* AP_PASSWORD = "esp32-config";

static const uint32_t CONNECT_TIMEOUT_MS   = 20000; // give up on saved/entered Wi-Fi
static const uint32_t AP_SHUTDOWN_DELAY_MS = 10000; // let the browser read the IP first

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static WebServer   server(80);
static Preferences preferences;

enum ConnState { ST_IDLE, ST_CONNECTING, ST_CONNECTED, ST_FAILED };
static ConnState connState = ST_IDLE;
static const char* STATE_NAMES[] = {"idle", "connecting", "connected", "failed"};

static String   pendingSSID;
static String   pendingPassword;
static uint32_t connectionStartedAt = 0;
static uint32_t connectedAt = 0;
static bool     portalActive = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static String formatUptime() {
  uint32_t s = millis() / 1000;
  char buf[48];
  snprintf(buf, sizeof(buf), "%lud, %02lu:%02lu:%02lu",
           (unsigned long)(s / 86400),
           (unsigned long)((s / 3600) % 24),
           (unsigned long)((s / 60) % 60),
           (unsigned long)(s % 60));
  return String(buf);
}

static void sendJson(JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

// Map the qualitative air-quality label to a 0-100 score for the dashboard.
static int airQualityScore(float rsRo) {
  const char* label = airQualityLabel(rsRo);
  if (strcmp(label, "Great") == 0) return 95;
  if (strcmp(label, "Good") == 0)  return 75;
  if (strcmp(label, "Bad") == 0)   return 45;
  if (strcmp(label, "Worse") == 0) return 20;
  return 0;
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
  server.send_P(200, "text/html", WEB_PAGE_HTML);
}

static void handleNetworks() {
  int count = WiFi.scanNetworks(false, true);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    if (WiFi.SSID(i).isEmpty()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  sendJson(doc);
}

static void handleConnect() {
  pendingSSID = server.arg("ssid");
  pendingPassword = server.arg("password");

  if (pendingSSID.isEmpty()) {
    server.send(400, "application/json",
                "{\"ok\":false,\"message\":\"SSID required\"}");
    return;
  }

  connState = ST_CONNECTING;
  connectionStartedAt = millis();

  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str());

  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleStatus() {
  JsonDocument doc;
  doc["state"] = STATE_NAMES[connState];
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = WiFi.localIP().toString();
  sendJson(doc);
}

static void handleInfo() {
  bool ok = WiFi.status() == WL_CONNECTED;

  JsonDocument doc;
  doc["status"] = ok ? "Connected" : "Disconnected";
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = ok ? WiFi.RSSI() : 0;
  doc["mac"] = WiFi.macAddress();
  doc["chip"] = ESP.getChipModel();
  doc["cpu"] = getCpuFrequencyMhz();
  doc["flash"] = ESP.getFlashChipSize();
  doc["psram"] = ESP.getPsramSize();
  doc["uptime"] = formatUptime();
  doc["heap"] = ESP.getFreeHeap();
  doc["freePsram"] = ESP.getFreePsram();
  sendJson(doc);
}

static void handleSensors() {
  JsonDocument doc;

  if (dhtValid) {
    doc["temperature"] = dhtTemperature;
    doc["humidity"] = dhtHumidity;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }

  doc["noise"] = micLevel;        // mic decision level (same as the OLED)
  doc["chemicals"] = gasVoltage;  // MQ135 module voltage

  if (isnan(gasRsRo)) {
    doc["airQuality"] = nullptr;
  } else {
    doc["airQuality"] = airQualityScore(gasRsRo);
  }

  sendJson(doc);
}

static void handleForgetWiFi() {
  preferences.begin("wifi-config", false);
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();

  server.send(200, "application/json",
              "{\"ok\":true,\"message\":\"Credentials removed; restarting\"}");

  delay(300);
  ESP.restart();
}

static void configureWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/networks", HTTP_GET, handleNetworks);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/sensors", HTTP_GET, handleSensors);
  server.on("/forget", HTTP_POST, handleForgetWiFi);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

// ---------------------------------------------------------------------------
// Portal / connection
// ---------------------------------------------------------------------------
static void startPortal() {
  connState = ST_IDLE;
  portalActive = true;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.println();
  Serial.println("Wi-Fi setup mode active.");
  Serial.printf("AP SSID: %s\n", AP_SSID);
  Serial.printf("Open http://%s in your browser.\n",
                WiFi.softAPIP().toString().c_str());
}

static bool connectSavedWiFi() {
  preferences.begin("wifi-config", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();

  if (ssid.isEmpty()) return false;

  Serial.printf("Connecting to saved Wi-Fi '%s'...\n", ssid.c_str());

  connState = ST_CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < CONNECT_TIMEOUT_MS) {
    delay(10);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Saved Wi-Fi network not reachable.");
    WiFi.disconnect(true, false);
    connState = ST_FAILED;
    return false;
  }

  connState = ST_CONNECTED;
  connectedAt = millis();

  Serial.printf("Wi-Fi connected: %s  IP: %s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void webSetup() {
  WiFi.persistent(false);

  if (!connectSavedWiFi()) {
    startPortal();
  }

  configureWebServer();
  server.begin();
}

void webLoop() {
  server.handleClient();

  // Supervise a connection started from the portal
  if (connState == ST_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      connState = ST_CONNECTED;
      connectedAt = millis();

      if (pendingSSID.length() > 0) {
        preferences.begin("wifi-config", false);
        preferences.putString("ssid", pendingSSID);
        preferences.putString("password", pendingPassword);
        preferences.end();
      }
      pendingPassword = "";

      Serial.printf("Wi-Fi connected: %s  IP: %s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else if (millis() - connectionStartedAt >= CONNECT_TIMEOUT_MS) {
      connState = ST_FAILED;
      pendingPassword = "";
      Serial.println("Wi-Fi connection timed out.");
    }
  }

  // The configuration AP is only shut down after connecting through it
  if (portalActive && connState == ST_CONNECTED &&
      millis() - connectedAt >= AP_SHUTDOWN_DELAY_MS) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portalActive = false;

    Serial.println("Setup AP disabled.");
    Serial.printf("Dashboard: http://%s\n", WiFi.localIP().toString().c_str());
  }
}
