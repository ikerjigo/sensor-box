
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>

// --------------------------------------------------
// Configuration
// --------------------------------------------------

#define RGB_LED_PIN 48

static const char *AP_SSID = "ESP32-Setup";
static const char *AP_PASSWORD = "esp32-config";

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t AP_SHUTDOWN_DELAY_MS = 10000;
static const uint32_t LED_INTERVAL_MS = 500;

// --------------------------------------------------
// Globals
// --------------------------------------------------

WebServer server(80);
Preferences preferences;

enum ConnectionState {
    STATE_IDLE,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_FAILED
};

ConnectionState connectionState = STATE_IDLE;

String pendingSSID;
String pendingPassword;

uint32_t connectionStartedAt = 0;
uint32_t connectedAt = 0;
uint32_t lastLedUpdate = 0;

bool portalActive = false;
bool credentialsSaved = false;
bool ledOn = false;

// --------------------------------------------------
// RGB LED status
// --------------------------------------------------


void updateStatusLED() {
    uint32_t now = millis();

    if (now - lastLedUpdate >= LED_INTERVAL_MS) {
        lastLedUpdate = now;
        ledOn = !ledOn;
    }

    if (connectionState == STATE_CONNECTED) {
        // Dim blue: connected
        neopixelWrite(RGB_LED_PIN, 0, 0, 15);
    }
    else if (connectionState == STATE_CONNECTING) {
        // Dim flashing yellow: connecting
        if (ledOn) {
            neopixelWrite(RGB_LED_PIN, 15, 6, 0);
        } else {
            neopixelWrite(RGB_LED_PIN, 0, 0, 0);
        }
    }
    else if (portalActive && connectionState == STATE_IDLE) {
        // Dim flashing red: setup AP active
        if (ledOn) {
            neopixelWrite(RGB_LED_PIN, 15, 0, 0);
        } else {
            neopixelWrite(RGB_LED_PIN, 0, 0, 0);
        }
    }
    else if (connectionState == STATE_FAILED) {
        // Dim red: connection failed
        neopixelWrite(RGB_LED_PIN, 15, 0, 0);
    }
    else {
        neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    }
}


// --------------------------------------------------
// Helpers
// --------------------------------------------------

String connectionStateName() {
    switch (connectionState) {
        case STATE_IDLE:
            return "idle";
        case STATE_CONNECTING:
            return "connecting";
        case STATE_CONNECTED:
            return "connected";
        case STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

String jsonEscape(const String &input) {
    String output;
    output.reserve(input.length() + 8);

    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];

        switch (c) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if ((uint8_t)c >= 0x20) {
                    output += c;
                }
                break;
        }
    }

    return output;
}

String formatUptime() {
    uint32_t seconds = millis() / 1000;

    uint32_t days = seconds / 86400;
    seconds %= 86400;

    uint32_t hours = seconds / 3600;
    seconds %= 3600;

    uint32_t minutes = seconds / 60;
    seconds %= 60;

    char buffer[48];

    snprintf(
        buffer,
        sizeof(buffer),
        "%lu days, %02lu:%02lu:%02lu",
        (unsigned long)days,
        (unsigned long)hours,
        (unsigned long)minutes,
        (unsigned long)seconds
    );

    return String(buffer);
}

String makePage(const String &title, const String &content) {
    String html;
    html.reserve(7000);

    html += R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>)rawliteral";

    html += title;

    html += R"rawliteral(</title>
<style>
:root {
    color-scheme: light;
    --bg: #f3f5f8;
    --card: #ffffff;
    --text: #202633;
    --muted: #687386;
    --border: #dce1e8;
    --accent: #3768e8;
    --input: #ffffff;
}
body.dark {
    color-scheme: dark;
    --bg: #11151c;
    --card: #1b222d;
    --text: #e8edf5;
    --muted: #a1adbf;
    --border: #343e4d;
    --accent: #7094ff;
    --input: #151b24;
}
* { box-sizing: border-box; }
body {
    margin: 0;
    padding: 24px 14px;
    background: var(--bg);
    color: var(--text);
    font-family: system-ui, sans-serif;
}
main { max-width: 650px; margin: auto; }
header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 22px;
}
h1 { font-size: 1.55rem; margin: 0; }
h2 { font-size: 1.1rem; margin-top: 0; }
p { line-height: 1.5; }
.card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 14px;
    padding: 20px;
    margin-bottom: 16px;
}
.muted { color: var(--muted); }
label {
    display: block;
    margin: 15px 0 7px;
    font-weight: 600;
}
input, select, button {
    width: 100%;
    padding: 12px;
    border-radius: 9px;
    border: 1px solid var(--border);
    background: var(--input);
    color: var(--text);
    font: inherit;
}
button {
    background: var(--accent);
    color: white;
    border: none;
    cursor: pointer;
    margin-top: 14px;
    font-weight: 600;
}
button.secondary {
    background: transparent;
    color: var(--text);
    border: 1px solid var(--border);
}
button.danger { background: #bd3030; }
button:disabled { opacity: .55; cursor: wait; }
.grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
    gap: 10px;
}
.stat {
    padding: 13px;
    border: 1px solid var(--border);
    border-radius: 10px;
    min-width: 0;
}
.stat .value {
    font-size: 1.05rem;
    font-weight: 650;
    overflow-wrap: anywhere;
    margin-top: 6px;
}
.status { font-weight: 650; }
a { color: var(--accent); }
</style>
<script>
function applyTheme() {
    document.body.classList.toggle(
        'dark',
        localStorage.getItem('esp32-theme') === 'dark'
    );
}
function toggleTheme() {
    const dark = !document.body.classList.contains('dark');
    localStorage.setItem('esp32-theme', dark ? 'dark' : 'light');
    applyTheme();
}
document.addEventListener('DOMContentLoaded', applyTheme);
</script>
</head>
<body>
<main>
<header>
    <h1>ESP32 Dashboard</h1>
    <button class="secondary" style="width:auto;margin:0"
        onclick="toggleTheme()">Theme</button>
</header>
)rawliteral";

    html += content;

    html += R"rawliteral(
<p class="muted" style="text-align:center;font-size:.85rem">
ESP32-S3 · Local web interface
</p>
</main>
</body>
</html>
)rawliteral";

    return html;
}

// --------------------------------------------------
// Setup page
// --------------------------------------------------

String makeSetupPage() {
    String content = R"rawliteral(
<section class="card">
    <h2>Wi-Fi configuration</h2>
    <p class="muted">
        Select your network and enter its password.
        The setup access point will shut down after a successful connection.
    </p>

    <form id="wifiForm">
        <label for="ssid">Wi-Fi network</label>
        <select id="ssid" name="ssid" required>
            <option value="">Scan for networks</option>
        </select>

        <button type="button" class="secondary"
            onclick="scanNetworks()">Scan networks</button>

        <label for="password">Wi-Fi password</label>
        <input id="password" name="password" type="password"
            autocomplete="current-password"
            placeholder="Enter password">

        <button id="connectButton" type="submit">Connect</button>
    </form>

    <p id="status" class="status">Setup access point active</p>
    <p class="muted">
        If the connection succeeds, find the ESP32's IP address
        in your router or the serial monitor.
    </p>
</section>

<script>
const statusEl = document.getElementById('status');
const connectButton = document.getElementById('connectButton');
const ssidSelect = document.getElementById('ssid');

async function scanNetworks() {
    statusEl.textContent = 'Scanning for networks...';

    try {
        const response = await fetch('/networks');
        const networks = await response.json();

        ssidSelect.innerHTML = '';

        if (!networks.length) {
            ssidSelect.add(new Option('No networks found', ''));
        }

        networks.forEach(network => {
            const option = new Option(
                network.ssid + ' (' + network.rssi + ' dBm)',
                network.ssid
            );
            ssidSelect.add(option);
        });

        statusEl.textContent = 'Network scan complete.';
    } catch (error) {
        statusEl.textContent = 'Could not scan for networks.';
    }
}

document.getElementById('wifiForm').addEventListener('submit', async event => {
    event.preventDefault();

    const ssid = ssidSelect.value;
    const password = document.getElementById('password').value;

    if (!ssid) {
        statusEl.textContent = 'Select a Wi-Fi network first.';
        return;
    }

    connectButton.disabled = true;
    statusEl.textContent = 'Connecting...';

    try {
        const body = new URLSearchParams();
        body.set('ssid', ssid);
        body.set('password', password);

        const response = await fetch('/connect', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body
        });

        const result = await response.json();

        if (!result.ok) {
            statusEl.textContent = result.message || 'Could not start connection.';
            connectButton.disabled = false;
            return;
        }

        pollStatus();
    } catch (error) {
        statusEl.textContent = 'Connection request failed.';
        connectButton.disabled = false;
    }
});

async function pollStatus() {
    try {
        const response = await fetch('/status');
        const result = await response.json();

        if (result.state === 'connected') {
            statusEl.textContent =
                'Connected. IP address: ' + result.ip +
                '. Open this address after the setup network closes.';
            return;
        }

        if (result.state === 'failed') {
            statusEl.textContent =
                'Connection failed. Check your password and try again.';
            connectButton.disabled = false;
            return;
        }

        statusEl.textContent = 'Connecting to Wi-Fi...';
        setTimeout(pollStatus, 1500);
    } catch (error) {
        statusEl.textContent =
            'Connection status unavailable. Check the serial monitor.';
        connectButton.disabled = false;
    }
}

scanNetworks();
</script>
)rawliteral";

    return makePage("ESP32 Wi-Fi Setup", content);
}

// --------------------------------------------------
// Dashboard page
// --------------------------------------------------

String makeDashboard() {
    String content = R"rawliteral(
<section class="card">
    <h2>Network</h2>
    <div class="grid">
        <div class="stat">
            <div class="muted">Status</div>
            <div class="value" id="wifiStatus">Connected</div>
        </div>
        <div class="stat">
            <div class="muted">SSID</div>
            <div class="value" id="ssid">-</div>
        </div>
        <div class="stat">
            <div class="muted">IP address</div>
            <div class="value" id="ip">-</div>
        </div>
        <div class="stat">
            <div class="muted">Signal strength</div>
            <div class="value" id="rssi">-</div>
        </div>
        <div class="stat">
            <div class="muted">MAC address</div>
            <div class="value" id="mac">-</div>
        </div>
    </div>
</section>

<section class="card">
    <h2>Hardware</h2>
    <div class="grid">
        <div class="stat">
            <div class="muted">Chip</div>
            <div class="value" id="chip">-</div>
        </div>
        <div class="stat">
            <div class="muted">CPU frequency</div>
            <div class="value" id="cpu">-</div>
        </div>
        <div class="stat">
            <div class="muted">Flash size</div>
            <div class="value" id="flash">-</div>
        </div>
        <div class="stat">
            <div class="muted">PSRAM size</div>
            <div class="value" id="psram">-</div>
        </div>
    </div>
</section>

<section class="card">
    <h2>System</h2>
    <div class="grid">
        <div class="stat">
            <div class="muted">Uptime</div>
            <div class="value" id="uptime">-</div>
        </div>
        <div class="stat">
            <div class="muted">Free heap</div>
            <div class="value" id="heap">-</div>
        </div>
        <div class="stat">
            <div class="muted">Free PSRAM</div>
            <div class="value" id="freePsram">-</div>
        </div>
    </div>
</section>

<section class="card">
    <h2>Wi-Fi settings</h2>
    <p class="muted">
        Forgetting the saved network erases the stored credentials
        and restarts the ESP32 in setup mode.
    </p>
    <button class="danger" onclick="forgetWiFi()">Forget Wi-Fi</button>
</section>

<script>
function setText(id, value) {
    document.getElementById(id).textContent = value;
}

function bytes(value) {
    if (!value) return '0 MB';
    return (value / (1024 * 1024)).toFixed(2) + ' MB';
}

async function refreshInfo() {
    try {
        const response = await fetch('/info');
        const d = await response.json();

        setText('wifiStatus', d.status);
        setText('ssid', d.ssid);
        setText('ip', d.ip);
        setText('rssi', d.rssi + ' dBm');
        setText('mac', d.mac);
        setText('chip', d.chip);
        setText('cpu', d.cpu + ' MHz');
        setText('flash', bytes(d.flash));
        setText('psram', bytes(d.psram));
        setText('uptime', d.uptime);
        setText('heap', bytes(d.heap));
        setText('freePsram', bytes(d.freePsram));
    } catch (error) {
        setText('wifiStatus', 'Unavailable');
    }
}

async function forgetWiFi() {
    if (!confirm('Forget the saved Wi-Fi network and restart?')) return;

    try {
        await fetch('/forget', {method: 'POST'});
        document.body.innerHTML =
            '<main><h1>Restarting...</h1>' +
            '<p>Reconnect to ESP32-Setup and open http://192.168.4.1</p></main>';
    } catch (error) {
        document.body.innerHTML =
            '<main><h1>Restarting...</h1>' +
            '<p>Reconnect to the ESP32 setup network.</p></main>';
    }
}

refreshInfo();
setInterval(refreshInfo, 3000);
</script>
)rawliteral";

    return makePage("ESP32 Dashboard", content);
}

// --------------------------------------------------
// HTTP handlers
// --------------------------------------------------

void handleRoot() {
    if (WiFi.status() == WL_CONNECTED) {
        server.send(200, "text/html", makeDashboard());
    } else {
        server.send(200, "text/html", makeSetupPage());
    }
}

void handleNetworks() {
    int count = WiFi.scanNetworks(false, true);

    String json = "[";
    bool first = true;

    for (int i = 0; i < count; i++) {
        if (WiFi.SSID(i).length() == 0) {
            continue;
        }

        if (!first) json += ",";
        first = false;

        json += "{\"ssid\":\"";
        json += jsonEscape(WiFi.SSID(i));
        json += "\",\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += "}";
    }

    json += "]";

    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

void handleConnect() {
    if (!server.hasArg("ssid")) {
        server.send(
            400,
            "application/json",
            "{\"ok\":false,\"message\":\"Missing SSID\"}"
        );
        return;
    }

    pendingSSID = server.arg("ssid");
    pendingPassword = server.arg("password");

    if (pendingSSID.length() == 0) {
        server.send(
            400,
            "application/json",
            "{\"ok\":false,\"message\":\"SSID cannot be empty\"}"
        );
        return;
    }

    connectionState = STATE_CONNECTING;
    credentialsSaved = false;
    connectionStartedAt = millis();

    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false, false);
    WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str());

    server.send(
        200,
        "application/json",
        "{\"ok\":true,\"message\":\"Connection started\"}"
    );
}

void handleStatus() {
    String json = "{\"state\":\"";
    json += connectionStateName();
    json += "\",\"ssid\":\"";
    json += jsonEscape(WiFi.SSID());
    json += "\",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"}";

    server.send(200, "application/json", json);
}

void handleInfo() {
    String json = "{";

    json += "\"status\":\"";
    json += WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
    json += "\",";

    json += "\"ssid\":\"";
    json += jsonEscape(WiFi.SSID());
    json += "\",";

    json += "\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\",";

    json += "\"rssi\":";
    json += String(
        WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0
    );
    json += ",";

    json += "\"mac\":\"";
    json += WiFi.macAddress();
    json += "\",";

    json += "\"chip\":\"ESP32-S3\",";
    json += "\"cpu\":";
    json += String(getCpuFrequencyMhz());
    json += ",";

    json += "\"flash\":";
    json += String(ESP.getFlashChipSize());
    json += ",";

    json += "\"psram\":";
    json += String(ESP.getPsramSize());
    json += ",";

    json += "\"uptime\":\"";
    json += formatUptime();
    json += "\",";

    json += "\"heap\":";
    json += String(ESP.getFreeHeap());
    json += ",";

    json += "\"freePsram\":";
    json += String(ESP.getFreePsram());

    json += "}";

    server.send(200, "application/json", json);
}

void handleForgetWiFi() {
    preferences.begin("wifi-config", false);
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();

    server.send(
        200,
        "application/json",
        "{\"ok\":true,\"message\":\"Credentials removed; restarting\"}"
    );

    delay(300);
    ESP.restart();
}

void configureWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/networks", HTTP_GET, handleNetworks);
    server.on("/connect", HTTP_POST, handleConnect);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/info", HTTP_GET, handleInfo);
    server.on("/forget", HTTP_POST, handleForgetWiFi);

    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
}

// --------------------------------------------------
// AP mode
// --------------------------------------------------

void startPortal() {
    connectionState = STATE_IDLE;
    portalActive = true;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    configureWebServer();
    server.begin();

    Serial.println();
    Serial.println("Wi-Fi setup mode active.");
    Serial.print("AP SSID: ");
    Serial.println(AP_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("Open http://192.168.4.1 in your browser.");
}

// --------------------------------------------------
// Saved Wi-Fi connection
// --------------------------------------------------

bool connectSavedWiFi() {
    preferences.begin("wifi-config", true);
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");
    preferences.end();

    if (ssid.length() == 0) {
        return false;
    }

    Serial.print("Connecting to saved network: ");
    Serial.println(ssid);

    connectionState = STATE_CONNECTING;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    uint32_t started = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < CONNECT_TIMEOUT_MS) {
        updateStatusLED();
        delay(10);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Saved Wi-Fi connection failed.");
        WiFi.disconnect(true, false);
        connectionState = STATE_FAILED;
        return false;
    }

    connectionState = STATE_CONNECTED;
    connectedAt = millis();

    configureWebServer();
    server.begin();

    Serial.println("Connected to saved Wi-Fi.");
    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.localIP());

    return true;
}

// --------------------------------------------------
// Setup
// --------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);

    // Initialise the onboard RGB LED.
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);

    Serial.println();
    Serial.println("ESP32-S3 starting...");

    if (!connectSavedWiFi()) {
        startPortal();
    }
}

// --------------------------------------------------
// Main loop
// --------------------------------------------------

void loop() {
    server.handleClient();
    updateStatusLED();

    if (connectionState == STATE_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            connectionState = STATE_CONNECTED;
            connectedAt = millis();

            if (pendingSSID.length() > 0) {
                preferences.begin("wifi-config", false);
                preferences.putString("ssid", pendingSSID);
                preferences.putString("password", pendingPassword);
                preferences.end();

                credentialsSaved = true;
            }

            pendingPassword = "";

            Serial.println("Wi-Fi connected.");
            Serial.print("SSID: ");
            Serial.println(WiFi.SSID());
            Serial.print("Dashboard IP: ");
            Serial.println(WiFi.localIP());
        }
        else if (millis() - connectionStartedAt >= CONNECT_TIMEOUT_MS) {
            connectionState = STATE_FAILED;
            pendingPassword = "";

            Serial.println("Wi-Fi connection timed out.");
        }
    }

    // Shut down the setup AP 10 seconds after successful connection.
    if (portalActive &&
        connectionState == STATE_CONNECTED &&
        millis() - connectedAt >= AP_SHUTDOWN_DELAY_MS) {

        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);

        portalActive = false;

        Serial.println("Setup AP disabled.");
        Serial.print("Dashboard remains available at: http://");
        Serial.println(WiFi.localIP());
    }

    delay(2);
}
