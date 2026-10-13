
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>

// ============================================================
// CONFIGURATION
// ============================================================

static const char *AP_SSID = "ESP32-Setup";

// Change this to a unique password of at least 8 characters.
static const char *AP_PASSWORD = "esp32-config";

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t AP_SHUTDOWN_DELAY_MS = 10000;

// ============================================================
// GLOBAL OBJECTS AND STATE
// ============================================================

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

bool portalActive = false;
bool credentialsSaved = false;

// ============================================================
// CONNECTION STATE NAME
// ============================================================

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
            return "idle";
    }
}

// ============================================================
// WIFI CONFIGURATION PAGE
// ============================================================

String makePage() {
    String page;
    page.reserve(7500);

    page += F(R"HTML(
<!DOCTYPE html>
<html lang="en" data-theme="dark">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Wi-Fi Setup</title>

<style>
:root {
    color-scheme: dark;
    --bg: #0b1120;
    --panel: #151e30;
    --field: #1d293d;
    --text: #e5edf8;
    --muted: #93a4bb;
    --border: #2b3a50;
    --accent: #3b82f6;
    --status-bg: #1d293d;
    --secondary: #334155;
    --shadow: 0 8px 30px #00000020;
}

html[data-theme="light"] {
    color-scheme: light;
    --bg: #f1f5f9;
    --panel: #ffffff;
    --field: #ffffff;
    --text: #172033;
    --muted: #64748b;
    --border: #dbe3ee;
    --accent: #2563eb;
    --status-bg: #f1f5f9;
    --secondary: #4b5563;
    --shadow: 0 8px 30px #0f172a0c;
}

* { box-sizing: border-box; }

body {
    font-family: system-ui, sans-serif;
    max-width: 560px;
    margin: 40px auto;
    padding: 0 18px;
    background: var(--bg);
    color: var(--text);
    transition: background .2s, color .2s;
}

main {
    background: var(--panel);
    border: 1px solid var(--border);
    padding: 24px;
    border-radius: 16px;
    box-shadow: var(--shadow);
}

header {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: 12px;
    flex-wrap: wrap;
    margin-bottom: 20px;
}

h1 {
    font-size: 1.55rem;
    letter-spacing: -.03em;
    margin: 0 0 8px;
}

p {
    color: var(--muted);
    line-height: 1.5;
}

.theme-toggle {
    flex-shrink: 0;
    padding: 9px 12px;
    border: 1px solid var(--border);
    border-radius: 9px;
    background: var(--field);
    color: var(--text);
    font: inherit;
    cursor: pointer;
}

.theme-toggle:hover { border-color: var(--accent); }

label {
    display: block;
    margin: 18px 0 7px;
    font-weight: 600;
    font-size: .92rem;
}

select, input, .action-button {
    width: 100%;
    padding: 12px;
    border: 1px solid var(--border);
    border-radius: 9px;
    font: inherit;
}

select, input {
    background: var(--field);
    color: var(--text);
}

input::placeholder { color: var(--muted); }

.action-button {
    margin-top: 12px;
    background: var(--accent);
    color: white;
    border: none;
    cursor: pointer;
    font-weight: 600;
}

.action-button:hover { filter: brightness(1.08); }
.secondary { background: var(--secondary); }

button:disabled {
    opacity: .5;
    cursor: wait;
}

#status {
    margin-top: 20px;
    padding: 13px;
    background: var(--status-bg);
    border: 1px solid var(--border);
    border-radius: 10px;
    line-height: 1.5;
    overflow-wrap: anywhere;
}

.muted {
    color: var(--muted);
    font-size: .85rem;
    margin-bottom: 0;
}

@media (max-width: 420px) {
    body { margin: 20px auto; padding: 0 12px; }
    main { padding: 18px; }
    h1 { font-size: 1.35rem; }
}
</style>
</head>

<body>
<main>
    <header>
        <div>
            <h1>ESP32 Wi-Fi Setup</h1>
            <p>Select your Wi-Fi network and enter its password.</p>
        </div>

        <button id="themeToggle" class="theme-toggle"
                type="button">Light mode</button>
    </header>

    <form id="wifiForm">
        <label for="ssid">Wi-Fi network</label>
        <select id="ssid" name="ssid" required>
            <option value="">Scanning networks...</option>
        </select>

        <button type="button" id="scanButton"
                class="action-button secondary">
            Refresh networks
        </button>

        <label for="password">Wi-Fi password</label>
        <input id="password" name="password" type="password"
               autocomplete="current-password"
               placeholder="Enter password">

        <button id="connectButton" class="action-button"
                type="submit">Connect</button>
    </form>

    <div id="status" role="status" aria-live="polite">
        Initializing...
    </div>

    <p class="muted">Supports 2.4 GHz Wi-Fi networks.</p>
</main>

<script>
// ---------------- Theme ----------------

const root = document.documentElement;
const themeToggle = document.getElementById('themeToggle');

function applyTheme(theme) {
    root.dataset.theme = theme;
    themeToggle.textContent =
        theme === 'dark' ? 'Light mode' : 'Dark mode';
}

let savedTheme = 'dark';

try {
    const stored = localStorage.getItem('esp32-theme');
    if (stored === 'light' || stored === 'dark') {
        savedTheme = stored;
    }
} catch (error) {}

applyTheme(savedTheme);

themeToggle.addEventListener('click', () => {
    const next = root.dataset.theme === 'dark' ? 'light' : 'dark';
    applyTheme(next);

    try {
        localStorage.setItem('esp32-theme', next);
    } catch (error) {}
});

// ---------------- Wi-Fi setup ----------------

const form = document.getElementById('wifiForm');
const ssidSelect = document.getElementById('ssid');
const passwordInput = document.getElementById('password');
const statusBox = document.getElementById('status');
const connectButton = document.getElementById('connectButton');
const scanButton = document.getElementById('scanButton');

let connecting = false;

async function refreshNetworks() {
    statusBox.textContent = 'Scanning for networks...';
    scanButton.disabled = true;

    try {
        const response = await fetch('/networks', {
            cache: 'no-store'
        });

        if (!response.ok) throw new Error('Scan failed');

        const networks = await response.json();
        ssidSelect.replaceChildren();

        if (networks.length === 0) {
            const option = document.createElement('option');
            option.value = '';
            option.textContent = 'No networks found';
            ssidSelect.appendChild(option);
        }

        for (const network of networks) {
            const option = document.createElement('option');
            option.value = network.ssid;
            option.textContent =
                network.ssid + ' (' + network.rssi + ' dBm)' +
                (network.open ? ' — open' : '');

            ssidSelect.appendChild(option);
        }

        statusBox.textContent = networks.length
            ? 'Select a network to continue.'
            : 'No networks found. Try scanning again.';
    } catch (error) {
        statusBox.textContent =
            'Could not scan networks. Refresh and try again.';
    } finally {
        scanButton.disabled = false;
    }
}

scanButton.addEventListener('click', refreshNetworks);

form.addEventListener('submit', async (event) => {
    event.preventDefault();

    if (!ssidSelect.value) {
        statusBox.textContent = 'Select a Wi-Fi network first.';
        return;
    }

    connecting = true;
    connectButton.disabled = true;
    statusBox.textContent = 'Attempting to connect...';

    try {
        const body = new URLSearchParams();
        body.set('ssid', ssidSelect.value);
        body.set('password', passwordInput.value);

        const response = await fetch('/connect', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded'
            },
            body
        });

        const result = await response.json();

        if (!response.ok) {
            throw new Error(result.message || 'Request failed');
        }

        statusBox.textContent = result.message;
        passwordInput.value = '';
    } catch (error) {
        statusBox.textContent =
            'Could not submit credentials. Try again.';
        connecting = false;
        connectButton.disabled = false;
    }
});

async function pollStatus() {
    try {
        const response = await fetch('/status', {
            cache: 'no-store'
        });

        if (!response.ok) return;

        const result = await response.json();

        if (result.state === 'connecting') {
            statusBox.textContent =
                'Connecting to ' + result.ssid + '...';
        } else if (result.state === 'connected') {
            statusBox.textContent =
                'Connected! IP: ' + result.ip +
                '. The setup hotspot will shut down shortly.';

            connectButton.disabled = true;
            scanButton.disabled = true;
            connecting = false;
        } else if (result.state === 'failed') {
            statusBox.textContent =
                'Connection failed. Check the password or signal, then try again.';

            connecting = false;
            connectButton.disabled = false;
        } else if (result.state === 'idle' && !connecting) {
            statusBox.textContent = 'Select a network to continue.';
        }
    } catch (error) {
        // The setup hotspot may have shut down.
    }
}

refreshNetworks();
setInterval(pollStatus, 1500);
</script>
</body>
</html>
)HTML");

    return page;
}

// ============================================================
// DEVICE DASHBOARD
// ============================================================

String makeDashboard() {
    String page;
    page.reserve(8500);

    page += F(R"HTML(
<!DOCTYPE html>
<html lang="en" data-theme="dark">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Dashboard</title>

<style>
:root {
    color-scheme: dark;
    --bg: #0b1120;
    --panel: #151e30;
    --card: #1d293d;
    --text: #e5edf8;
    --muted: #93a4bb;
    --border: #2b3a50;
    --accent: #60a5fa;
    --success: #4ade80;
    --shadow: 0 8px 30px #00000020;
}

html[data-theme="light"] {
    color-scheme: light;
    --bg: #f1f5f9;
    --panel: #ffffff;
    --card: #f1f5f9;
    --text: #172033;
    --muted: #64748b;
    --border: #dbe3ee;
    --accent: #2563eb;
    --success: #15803d;
    --shadow: 0 8px 30px #0f172a0c;
}

* { box-sizing: border-box; }

body {
    font-family: system-ui, sans-serif;
    max-width: 780px;
    margin: 0 auto;
    padding: 30px 18px;
    background: var(--bg);
    color: var(--text);
    transition: background .2s, color .2s;
}

header, section {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 22px;
    margin-bottom: 16px;
    box-shadow: var(--shadow);
}

.topbar {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 14px;
    flex-wrap: wrap;
}

h1 {
    font-size: 1.7rem;
    letter-spacing: -.04em;
    margin: 0 0 8px;
}

h2 { font-size: 1rem; margin: 0 0 16px; }
p { color: var(--muted); line-height: 1.5; }
.subtitle { margin: 0; font-size: .95rem; }

.actions {
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
}

.actions button {
    font: inherit;
    cursor: pointer;
    border-radius: 10px;
    padding: 10px 14px;
}

.theme-toggle {
    background: var(--card);
    border: 1px solid var(--border);
    color: var(--text);
}

.theme-toggle:hover { border-color: var(--accent); }

.forget-button {
    background: transparent;
    color: #f87171;
    border: 1px solid #7f3038;
}

.forget-button:hover { background: #7f303822; }

.actions button:disabled {
    opacity: .5;
    cursor: wait;
}

.status {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    margin-top: 20px;
    padding: 8px 12px;
    border-radius: 999px;
    background: var(--card);
    color: var(--success);
    font-size: .9rem;
}

.dot {
    width: 8px;
    height: 8px;
    background: var(--success);
    border-radius: 50%;
}

.grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 12px;
}

.item {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 16px;
    min-width: 0;
}

.label {
    color: var(--muted);
    font-size: .82rem;
    margin-bottom: 8px;
}

.value {
    font-size: 1.12rem;
    font-weight: 650;
    overflow-wrap: anywhere;
}

footer {
    text-align: center;
    color: var(--muted);
    font-size: .82rem;
    padding: 10px;
}

@media (max-width: 420px) {
    body { padding: 18px 12px; }
    header, section { padding: 17px; }
    .grid { grid-template-columns: 1fr 1fr; }
    .value { font-size: 1rem; }
}
</style>
</head>

<body>
<header>
    <div class="topbar">
        <div>
            <h1>ESP32 Dashboard</h1>
            <p class="subtitle">Device status and system information</p>
        </div>

        <div class="actions">
            <button id="themeToggle" class="theme-toggle"
                    type="button">Light mode</button>
            <button id="forgetButton" class="forget-button"
                    type="button">Forget Wi-Fi</button>
        </div>
    </div>

    <div class="status">
        <span class="dot"></span>
        <span id="status">Connecting...</span>
    </div>
</header>

<section>
    <h2>Network</h2>
    <div class="grid">
        <div class="item">
            <div class="label">Wi-Fi network</div>
            <div class="value" id="ssid">--</div>
        </div>
        <div class="item">
            <div class="label">IP address</div>
            <div class="value" id="ip">--</div>
        </div>
        <div class="item">
            <div class="label">Signal strength</div>
            <div class="value" id="rssi">--</div>
        </div>
        <div class="item">
            <div class="label">Wi-Fi MAC address</div>
            <div class="value" id="mac">--</div>
        </div>
    </div>
</section>

<section>
    <h2>Hardware</h2>
    <div class="grid">
        <div class="item">
            <div class="label">Chip</div>
            <div class="value" id="chip">--</div>
        </div>
        <div class="item">
            <div class="label">CPU frequency</div>
            <div class="value" id="cpu">--</div>
        </div>
        <div class="item">
            <div class="label">Flash memory</div>
            <div class="value" id="flash">--</div>
        </div>
        <div class="item">
            <div class="label">PSRAM total</div>
            <div class="value" id="psram">--</div>
        </div>
    </div>
</section>

<section>
    <h2>System</h2>
    <div class="grid">
        <div class="item">
            <div class="label">Uptime</div>
            <div class="value" id="uptime">--</div>
        </div>
        <div class="item">
            <div class="label">Free heap</div>
            <div class="value" id="heap">--</div>
        </div>
        <div class="item">
            <div class="label">Free PSRAM</div>
            <div class="value" id="freepsram">--</div>
        </div>
    </div>
</section>

<footer>ESP32-S3 · Auto-refresh every 3 seconds</footer>

<script>
const root = document.documentElement;
const themeToggle = document.getElementById('themeToggle');

function applyTheme(theme) {
    root.dataset.theme = theme;
    themeToggle.textContent =
        theme === 'dark' ? 'Light mode' : 'Dark mode';
}

let savedTheme = 'dark';

try {
    const stored = localStorage.getItem('esp32-theme');
    if (stored === 'light' || stored === 'dark') {
        savedTheme = stored;
    }
} catch (error) {}

applyTheme(savedTheme);

themeToggle.addEventListener('click', () => {
    const next = root.dataset.theme === 'dark' ? 'light' : 'dark';
    applyTheme(next);

    try {
        localStorage.setItem('esp32-theme', next);
    } catch (error) {}
});

// ---------------- Forget Wi-Fi ----------------

document.getElementById('forgetButton').addEventListener('click', async () => {
    const confirmed = confirm(
        'Forget the saved Wi-Fi network? The ESP32 will restart ' +
        'and create its setup hotspot again.'
    );

    if (!confirmed) return;

    const button = document.getElementById('forgetButton');
    button.disabled = true;
    button.textContent = 'Forgetting Wi-Fi...';

    try {
        const response = await fetch('/forget', { method: 'POST' });
        if (!response.ok) throw new Error('Request failed');

        document.getElementById('status').textContent =
            'Credentials erased. Reconnect to ESP32-Setup.';
    } catch (error) {
        document.getElementById('status').textContent =
            'Device restarting or request failed. Check ESP32-Setup.';
    }
});

// ---------------- Dashboard updates ----------------

function setValue(id, value) {
    document.getElementById(id).textContent = value;
}

function formatBytes(bytes) {
    return (bytes / 1024).toFixed(1) + ' KB';
}

function formatUptime(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = seconds % 60;

    return days + 'd ' + hours + 'h ' +
           minutes + 'm ' + secs + 's';
}

async function updateDashboard() {
    try {
        const response = await fetch('/info', { cache: 'no-store' });
        if (!response.ok) throw new Error('Request failed');

        const d = await response.json();

        setValue('status', 'Connected');
        setValue('ssid', d.ssid);
        setValue('ip', d.ip);
        setValue('rssi', d.rssi + ' dBm');
        setValue('mac', d.mac);
        setValue('chip', d.chip);
        setValue('cpu', d.cpu + ' MHz');
        setValue('flash', formatBytes(d.flash));
        setValue('psram', formatBytes(d.psram));
        setValue('uptime', formatUptime(d.uptime));
        setValue('heap', formatBytes(d.heap));
        setValue('freepsram', formatBytes(d.freepsram));
    } catch (error) {
        setValue('status', 'Unable to reach the device');
    }
}

updateDashboard();
setInterval(updateDashboard, 3000);
</script>
</body>
</html>
)HTML");

    return page;
}

// ============================================================
// NETWORK SCANNING
// ============================================================

void handleNetworks() {
    if (!portalActive) {
        server.send(503, "application/json",
                    "{\"message\":\"Setup portal is disabled\"}");
        return;
    }

    int count = WiFi.scanNetworks(false, true);
    String json = "[";

    if (count > 0) {
        for (int i = 0; i < count; i++) {
            String ssid = WiFi.SSID(i);

            if (ssid.isEmpty()) continue;
            if (json.length() > 1) json += ',';

            json += "{\"ssid\":\"";

            for (size_t j = 0; j < ssid.length(); j++) {
                char c = ssid[j];

                if (c == '"' || c == '\\') {
                    json += '\\';
                    json += c;
                } else if ((uint8_t)c >= 0x20) {
                    json += c;
                }
            }

            json += "\",\"rssi\":";
            json += String(WiFi.RSSI(i));
            json += ",\"open\":";
            json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)
                        ? "true" : "false";
            json += '}';
        }
    }

    json += ']';
    WiFi.scanDelete();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// ============================================================
// CONNECT TO SELECTED NETWORK
// ============================================================

void handleConnect() {
    if (!portalActive) {
        server.send(503, "application/json",
                    "{\"message\":\"Setup portal is disabled\"}");
        return;
    }

    if (connectionState == STATE_CONNECTING) {
        server.send(409, "application/json",
                    "{\"message\":\"Connection already in progress\"}");
        return;
    }

    if (!server.hasArg("ssid") || !server.hasArg("password")) {
        server.send(400, "application/json",
                    "{\"message\":\"SSID and password are required\"}");
        return;
    }

    pendingSSID = server.arg("ssid");
    pendingPassword = server.arg("password");

    if (pendingSSID.isEmpty() || pendingSSID.length() > 32 ||
        pendingPassword.length() > 64) {
        server.send(400, "application/json",
                    "{\"message\":\"Invalid SSID or password length\"}");
        return;
    }

    connectionState = STATE_CONNECTING;
    credentialsSaved = false;
    connectionStartedAt = millis();

    WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str());

    Serial.printf("Connecting to Wi-Fi: %s\n", pendingSSID.c_str());

    server.send(200, "application/json",
                "{\"message\":\"Attempting to connect\"}");
}

// ============================================================
// SETUP CONNECTION STATUS
// ============================================================

void handleStatus() {
    String json = "{\"state\":\"";
    json += connectionStateName();
    json += "\",\"ssid\":\"";

    for (size_t i = 0; i < pendingSSID.length(); i++) {
        char c = pendingSSID[i];

        if (c == '"' || c == '\\') {
            json += '\\';
            json += c;
        } else if ((uint8_t)c >= 0x20) {
            json += c;
        }
    }

    json += "\",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// ============================================================
// DASHBOARD INFORMATION
// ============================================================

void handleInfo() {
    if (WiFi.status() != WL_CONNECTED) {
        server.send(503, "application/json",
                    "{\"error\":\"Wi-Fi disconnected\"}");
        return;
    }

    String json;
    json.reserve(400);

    json = "{\"ssid\":\"";

    String ssid = WiFi.SSID();

    for (size_t i = 0; i < ssid.length(); i++) {
        char c = ssid[i];

        if (c == '"' || c == '\\') {
            json += '\\';
            json += c;
        } else if ((uint8_t)c >= 0x20) {
            json += c;
        }
    }

    json += "\",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI());
    json += ",\"mac\":\"";
    json += WiFi.macAddress();
    json += "\",\"chip\":\"";
    json += ESP.getChipModel();
    json += "\",\"cpu\":";
    json += String(ESP.getCpuFreqMHz());
    json += ",\"flash\":";
    json += String(ESP.getFlashChipSize());
    json += ",\"psram\":";
    json += String(ESP.getPsramSize());
    json += ",\"freepsram\":";
    json += String(ESP.getFreePsram());
    json += ",\"heap\":";
    json += String(ESP.getFreeHeap());
    json += ",\"uptime\":";
    json += String(millis() / 1000);
    json += "}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// ============================================================
// FORGET SAVED WI-FI
// ============================================================

void handleForgetWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        server.send(503, "application/json",
                    "{\"error\":\"Device is not connected to Wi-Fi\"}");
        return;
    }

    Serial.println("Erasing saved Wi-Fi credentials...");

    preferences.begin("wifi-config", false);
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();

    server.send(200, "application/json",
                "{\"message\":\"Credentials erased. Restarting.\"}");

    delay(500);
    ESP.restart();
}

// ============================================================
// HTTP SERVER ROUTES
// ============================================================

void configureWebServer() {
    server.on("/", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "no-store");

        if (portalActive) {
            server.send(200, "text/html; charset=utf-8", makePage());
        } else {
            server.send(200, "text/html; charset=utf-8",
                        makeDashboard());
        }
    });

    server.on("/networks", HTTP_GET, handleNetworks);
    server.on("/connect", HTTP_POST, handleConnect);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/info", HTTP_GET, handleInfo);
    server.on("/forget", HTTP_POST, handleForgetWiFi);

    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
}

// ============================================================
// START SETUP HOTSPOT
// ============================================================

void startPortal() {
    WiFi.mode(WIFI_AP_STA);

    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        Serial.println("ERROR: Could not start setup hotspot.");
        return;
    }

    portalActive = true;
    connectionState = STATE_IDLE;

    configureWebServer();
    server.begin();

    Serial.println();
    Serial.println("Setup hotspot started.");
    Serial.printf("SSID: %s\n", AP_SSID);
    Serial.printf("Open: http://%s/\n",
                  WiFi.softAPIP().toString().c_str());
}

// ============================================================
// CONNECT USING SAVED CREDENTIALS
// ============================================================

bool connectSavedWiFi() {
    preferences.begin("wifi-config", true);

    String savedSSID = preferences.getString("ssid", "");
    String savedPassword = preferences.getString("password", "");

    preferences.end();

    if (savedSSID.isEmpty()) {
        Serial.println("No saved Wi-Fi credentials.");
        return false;
    }

    Serial.printf("Trying saved network: %s\n", savedSSID.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

    uint32_t started = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < CONNECT_TIMEOUT_MS) {
        delay(250);
        Serial.print('.');
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        pendingSSID = savedSSID;
        connectionState = STATE_CONNECTED;

        configureWebServer();
        server.begin();

        Serial.println("Connected using saved credentials.");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        Serial.println("Dashboard server started.");

        return true;
    }

    Serial.println("Saved network unavailable; starting setup.");

    WiFi.disconnect(true, false);
    return false;
}

// ============================================================
// ARDUINO SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("ESP32-S3 Wi-Fi provisioning");

    if (connectSavedWiFi()) {
        return;
    }

    startPortal();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    if (portalActive || connectionState == STATE_CONNECTED) {
        server.handleClient();
    }

    if (connectionState == STATE_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            connectionState = STATE_CONNECTED;
            connectedAt = millis();

            preferences.begin("wifi-config", false);
            preferences.putString("ssid", pendingSSID);
            preferences.putString("password", pendingPassword);
            preferences.end();

            credentialsSaved = true;
            pendingPassword = "";

            Serial.println();
            Serial.println("Wi-Fi connected successfully.");
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());
            Serial.println("Credentials saved.");

        } else if (millis() - connectionStartedAt >=
                   CONNECT_TIMEOUT_MS) {
            WiFi.disconnect(false, false);
            connectionState = STATE_FAILED;
            pendingPassword = "";

            Serial.println();
            Serial.println("Wi-Fi connection timed out.");
        }
    }

    if (portalActive &&
        connectionState == STATE_CONNECTED &&
        credentialsSaved &&
        millis() - connectedAt >= AP_SHUTDOWN_DELAY_MS) {

        Serial.println("Stopping setup hotspot.");

        // Keep the HTTP server running for the dashboard.
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);

        portalActive = false;

        Serial.println("Setup hotspot stopped.");
        Serial.print("Dashboard available at: http://");
        Serial.println(WiFi.localIP());
    }

    delay(2);
}
