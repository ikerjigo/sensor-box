/*
 ============================================================================
  ESP32-S3 · Portal de configuración Wi-Fi + Dashboard web
 ----------------------------------------------------------------------------
  Funcionamiento general:
   1. Al arrancar intenta conectarse a la red Wi-Fi guardada en la flash.
   2. Si no hay red guardada (o falla), crea su propia red "ESP32-Setup"
    (portal) para que el usuario elija su Wi-Fi y escriba la contraseña.
   3. Cuando hay conexión, sirve un dashboard con datos de red y hardware.
   4. Un LED RGB indica el estado en todo momento.

  Librerías necesarias: ArduinoJson v7 (Gestor de librerías).
 ============================================================================
*/

// ============================================================================
//  SECCIÓN 1 · INCLUDES
// ============================================================================



#include <Arduino.h>     // Funciones base: millis(), delay(), Serial, String, neopixelWrite(), para lo necesario de un microcontrolador
#include <WiFi.h>        // Conexión Wi-Fi (modo cliente) y punto de acceso (AP)
#include <WebServer.h>   // Servidor HTTP sencillo para el ESP32
#include <Preferences.h> // Guardar datos en la memoria flash (NVS); sobreviven a reinicios
#include <ArduinoJson.h> // Construir JSON sin concatenar strings a mano (escapa solo)



// ============================================================================
//  SECCIÓN 2 · CONFIGURACIÓN (constantes que puedes ajustar)
// ============================================================================



// Pin del LED RGB integrado (en muchas placas ESP32-S3 es el GPIO 48)
#define RGB_LED_PIN 48

// Nombre de la red Wi-Fi que crea el ESP32 en modo configuración
static const char *AP_SSID = "ESP32-Setup";

// Contraseña de esa red (mínimo 8 caracteres)
static const char *AP_PASSWORD = "esp32-config";

// Tiempo máximo (ms) para intentar conectar a una red antes de darse por vencido
static const uint32_t CONNECT_TIMEOUT_MS = 20000;

// Tiempo (ms) que se espera tras conectar antes de apagar el AP de configuración
// (da tiempo al navegador del usuario a mostrar la IP obtenida)
static const uint32_t AP_SHUTDOWN_DELAY_MS = 10000;

// Cada cuántos ms cambia la fase del parpadeo del LED
static const uint32_t LED_INTERVAL_MS = 500;



// ============================================================================
//  SECCIÓN 3 · VARIABLES GLOBALES Y ESTADOS
// ============================================================================



// Servidor HTTP escuchando en el puerto 80
WebServer server(80);

// Objeto para leer/escribir en la memoria flash (credenciales Wi-Fi)
Preferences preferences;

// Estados posibles de la conexión Wi-Fi (máquina de estados simple)
enum ConnectionState
{
    STATE_IDLE,       // Sin hacer nada (portal esperando al usuario)
    STATE_CONNECTING, // Intentando conectar a una red
    STATE_CONNECTED,  // Conectado correctamente
    STATE_FAILED      // La conexión falló o expiró el tiempo
};

// Estado actual de la conexión
ConnectionState connectionState = STATE_IDLE;

// Nombres de los estados en texto, en el mismo orden que el enum.
// Se usan para enviar el estado al navegador en formato JSON.
const char *STATE_NAMES[] = {"idle", "connecting", "connected", "failed"};

// SSID que el usuario envió desde el formulario (pendiente de verificar)
String pendingSSID;

// Contraseña enviada desde el formulario (solo se guarda si la conexión funciona)
String pendingPassword;

// Momento (millis) en que empezó el intento de conexión actual
uint32_t connectionStartedAt = 0;

// Momento (millis) en que se logró conectar
uint32_t connectedAt = 0;

// Última vez (millis) que cambió la fase del parpadeo del LED
uint32_t lastLedUpdate = 0;

// true mientras el AP de configuración (portal) está activo
bool portalActive = false;

// Fase actual del parpadeo: true = encendido, false = apagado
bool ledOn = false;



// ============================================================================
//  SECCIÓN 4 · LED RGB DE ESTADO
// ============================================================================



// Actualiza el LED según el estado. Se llama continuamente desde loop().
// Colores (valores bajos, 15 de 255, para que no deslumbre):
//   Azul fijo          -> conectado
//   Amarillo parpadea  -> conectando
//   Rojo fijo          -> conexión fallida
//   Rojo parpadea      -> portal de configuración activo
//   Apagado            -> cualquier otro caso
void updateStatusLED()
{
    uint32_t now = millis(); // Hora actual en ms desde el arranque

    // Cada LED_INTERVAL_MS invierte la fase del parpadeo (sin usar delay())
    if (now - lastLedUpdate >= LED_INTERVAL_MS)
    {
        lastLedUpdate = now; // Anota cuándo se hizo el último cambio
        ledOn = !ledOn;      // Alterna encendido/apagado
    }

    uint8_t r = 0, g = 0, b = 0; // Color por defecto: apagado

    if (connectionState == STATE_CONNECTED)
    {
        b = 15;
    } // Azul fijo
    else if (connectionState == STATE_CONNECTING)
    {
        if (ledOn)
        {
            r = 15;
            g = 6;
        }
    } // Amarillo parpadeante
    else if (connectionState == STATE_FAILED)
    {
        r = 15;
    } // Rojo fijo
    else if (portalActive)
    {
        if (ledOn)
            r = 15;
    } // Rojo parpadeante

    neopixelWrite(RGB_LED_PIN, r, g, b); // Escribe el color calculado en el LED
}



// ============================================================================
//  SECCIÓN 5 · FUNCIONES AUXILIARES
// ============================================================================



// Convierte el tiempo encendido (millis) a texto con formato "Xd, HH:MM:SS"
String formatUptime()
{
    uint32_t s = millis() / 1000; // Milisegundos -> segundos totales
    char buf[48];                 // Buffer donde se escribe el texto

    // %lu = unsigned long, %02lu = al menos 2 dígitos rellenando con ceros.
    // Se calculan días, horas (0-23), minutos (0-59) y segundos (0-59).
    snprintf(buf, sizeof(buf), "%lud, %02lu:%02lu:%02lu",
             (unsigned long)(s / 86400),
             (unsigned long)((s / 3600) % 24),
             (unsigned long)((s / 60) % 60),
             (unsigned long)(s % 60));

    return buf; // Se convierte automáticamente a String
}

// Serializa un documento JSON y lo envía al navegador como respuesta HTTP
void sendJson(JsonDocument &doc, int code = 200)
{
    String out;                                 // Texto JSON resultante
    serializeJson(doc, out);                    // Convierte el documento a texto
    server.send(code, "application/json", out); // Responde con el código y tipo adecuados
}

// ============================================================================
//  SECCIÓN 6 · PÁGINAS HTML
//  (El contenido entre R"rawliteral( ... )rawliteral" es HTML/CSS/JS que se
//   envía tal cual al navegador. Los literales "crudos" permiten escribir
//   varias líneas y comillas sin escapar nada.)
// ============================================================================

// Construye la página completa: cabecera + estilos + script de tema + contenido + pie.
//   title   -> texto de la pestaña del navegador
//   content -> HTML específico de cada página (setup o dashboard)
String makePage(const String &title, const String &content)
{
    String html;
    html.reserve(7000); // Reserva memoria de antemano (evita fragmentación)

    // Parte inicial: doctype, <head> y apertura de <title>
    html += R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>)rawliteral";

    html += title; // Inserta el título de la pestaña

    // Estilos CSS (tema claro/oscuro), script de cambio de tema y cabecera de la página
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

    html += content; // Inserta el contenido propio de cada página

    // Pie de página y cierre del HTML
    html += R"rawliteral(
<p class="muted" style="text-align:center;font-size:.85rem">
ESP32-S3 · Local web interface
</p>
</main>
</body>
</html>
)rawliteral";

    return html; // Devuelve la página completa
}

// Página de configuración Wi-Fi: escanear redes, elegir una, escribir la
// contraseña y conectar. Usa los endpoints /networks, /connect y /status.
String makeSetupPage()
{
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

    return makePage("ESP32 Wi-Fi Setup", content); // Envuelve el contenido en la página base
}

// Página del dashboard: muestra datos de red, hardware y sistema, y permite
// olvidar la Wi-Fi. Usa los endpoints /info (cada 3 s) y /forget.
String makeDashboard()
{
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

    return makePage("ESP32 Dashboard", content); // Envuelve el contenido en la página base
}

// ============================================================================
//  SECCIÓN 7 · MANEJADORES HTTP (uno por cada ruta del servidor)
// ============================================================================

// Ruta GET "/" -> muestra el dashboard si hay Wi-Fi, o la página de setup si no
void handleRoot()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        server.send(200, "text/html", makeDashboard()); // Conectado: dashboard
    }
    else
    {
        server.send(200, "text/html", makeSetupPage()); // No conectado: configuración
    }
}

// Ruta GET "/networks" -> escanea redes Wi-Fi y devuelve un array JSON
// con formato [{"ssid":"...","rssi":-60}, ...]
void handleNetworks()
{
    // Escaneo síncrono (false = bloquea hasta terminar), incluyendo redes ocultas (true)
    int count = WiFi.scanNetworks(false, true);

    JsonDocument doc;                    // Documento JSON (ArduinoJson v7)
    JsonArray arr = doc.to<JsonArray>(); // Lo usamos como array

    for (int i = 0; i < count; i++)
    {
        if (WiFi.SSID(i).isEmpty())
            continue; // Salta redes sin nombre (ocultas)

        JsonObject o = arr.add<JsonObject>(); // Añade un objeto al array
        o["ssid"] = WiFi.SSID(i);             // Nombre de la red
        o["rssi"] = WiFi.RSSI(i);             // Intensidad de señal en dBm
    }

    WiFi.scanDelete(); // Libera la memoria del resultado del escaneo
    sendJson(doc);     // Envía el JSON al navegador
}

// Ruta POST "/connect" -> recibe SSID y contraseña e inicia la conexión.
// La conexión no bloquea: se supervisa en loop() y el navegador consulta /status.
void handleConnect()
{
    pendingSSID = server.arg("ssid");         // Devuelve "" si el argumento no existe
    pendingPassword = server.arg("password"); // Contraseña tecleada por el usuario

    // Un SSID vacío (o ausente) es un error del cliente -> 400 Bad Request
    if (pendingSSID.isEmpty())
    {
        server.send(400, "application/json",
                    "{\"ok\":false,\"message\":\"SSID required\"}");
        return;
    }

    connectionState = STATE_CONNECTING; // Pasa al estado "conectando"
    connectionStartedAt = millis();     // Anota cuándo empezó (para el timeout)

    WiFi.mode(WIFI_AP_STA);                                   // Mantiene el AP y permite actuar como cliente
    WiFi.disconnect(false, false);                            // Corta intentos previos sin apagar radio ni borrar datos
    WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str()); // Inicia la conexión (no bloquea)

    server.send(200, "application/json", "{\"ok\":true}"); // Responde enseguida
}

// Ruta GET "/status" -> estado actual de la conexión, SSID e IP.
// La página de setup lo consulta cada 1,5 s para saber si ya se conectó.
void handleStatus()
{
    JsonDocument doc;
    doc["state"] = STATE_NAMES[connectionState]; // "idle", "connecting", "connected" o "failed"
    doc["ssid"] = WiFi.SSID();                   // Red a la que está conectado
    doc["ip"] = WiFi.localIP().toString();       // IP asignada por el router
    sendJson(doc);
}

// Ruta GET "/info" -> datos de red, hardware y sistema para el dashboard
void handleInfo()
{
    bool ok = WiFi.status() == WL_CONNECTED; // ¿Hay conexión Wi-Fi ahora mismo?

    JsonDocument doc;
    doc["status"] = ok ? "Connected" : "Disconnected"; // Estado de la Wi-Fi
    doc["ssid"] = WiFi.SSID();                         // Nombre de la red
    doc["ip"] = WiFi.localIP().toString();             // Dirección IP
    doc["rssi"] = ok ? WiFi.RSSI() : 0;                // Señal (0 si no hay conexión)
    doc["mac"] = WiFi.macAddress();                    // Dirección MAC
    doc["chip"] = ESP.getChipModel();                  // Modelo del chip (ej. ESP32-S3)
    doc["cpu"] = getCpuFrequencyMhz();                 // Frecuencia de CPU en MHz
    doc["flash"] = ESP.getFlashChipSize();             // Tamaño de la flash en bytes
    doc["psram"] = ESP.getPsramSize();                 // Tamaño de la PSRAM en bytes
    doc["uptime"] = formatUptime();                    // Tiempo encendido formateado
    doc["heap"] = ESP.getFreeHeap();                   // Memoria heap libre en bytes
    doc["freePsram"] = ESP.getFreePsram();             // PSRAM libre en bytes
    sendJson(doc);
}

// Ruta POST "/forget" -> borra las credenciales guardadas y reinicia en modo portal
void handleForgetWiFi()
{
    preferences.begin("wifi-config", false); // Abre el espacio "wifi-config" en modo escritura
    preferences.remove("ssid");              // Borra el SSID guardado
    preferences.remove("password");          // Borra la contraseña guardada
    preferences.end();                       // Cierra el espacio de almacenamiento

    server.send(200, "application/json",
                "{\"ok\":true,\"message\":\"Credentials removed; restarting\"}");

    delay(300);    // Da tiempo a que la respuesta llegue al navegador
    ESP.restart(); // Reinicia; sin credenciales arrancará el portal
}

// Registra todas las rutas del servidor y su función manejadora
void configureWebServer()
{
    server.on("/", HTTP_GET, handleRoot);              // Página principal
    server.on("/networks", HTTP_GET, handleNetworks);  // Escaneo de redes
    server.on("/connect", HTTP_POST, handleConnect);   // Iniciar conexión
    server.on("/status", HTTP_GET, handleStatus);      // Estado de la conexión
    server.on("/info", HTTP_GET, handleInfo);          // Datos del dashboard
    server.on("/forget", HTTP_POST, handleForgetWiFi); // Olvidar Wi-Fi

    // Cualquier otra ruta responde 404 (función lambda anónima)
    server.onNotFound([]()
                      { server.send(404, "text/plain", "Not found"); });
}

// ============================================================================
//  SECCIÓN 8 · MODO PORTAL (punto de acceso de configuración)
// ============================================================================

// Crea la red "ESP32-Setup" para que el usuario configure la Wi-Fi.
// El servidor web se arranca después, en setup().
void startPortal()
{
    connectionState = STATE_IDLE; // Sin intentos de conexión en curso
    portalActive = true;          // Marca el portal como activo (afecta al LED)

    WiFi.mode(WIFI_AP_STA);            // AP (portal) + STA (para poder probar redes)
    WiFi.softAP(AP_SSID, AP_PASSWORD); // Crea el punto de acceso

    // Mensajes informativos por el monitor serie
    Serial.println();
    Serial.println("Wi-Fi setup mode active.");
    Serial.print("AP SSID: ");
    Serial.println(AP_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP()); // Normalmente 192.168.4.1
    Serial.println("Open http://192.168.4.1 in your browser.");
}

// ============================================================================
//  SECCIÓN 9 · CONEXIÓN CON LA WI-FI GUARDADA
// ============================================================================

// Intenta conectar con las credenciales guardadas en flash.
// Devuelve true si conectó, false si no había datos o falló.
// Es bloqueante (espera hasta CONNECT_TIMEOUT_MS) pero mantiene el LED animado.
bool connectSavedWiFi()
{
    preferences.begin("wifi-config", true);                  // Abre en modo solo lectura (true)
    String ssid = preferences.getString("ssid", "");         // Lee el SSID ("" si no existe)
    String password = preferences.getString("password", ""); // Lee la contraseña
    preferences.end();                                       // Cierra el almacenamiento

    if (ssid.isEmpty())
        return false; // No hay red guardada -> usar portal

    Serial.print("Connecting to saved network: ");
    Serial.println(ssid);

    connectionState = STATE_CONNECTING;         // Estado "conectando" (LED amarillo)
    WiFi.mode(WIFI_STA);                        // Solo modo cliente
    WiFi.begin(ssid.c_str(), password.c_str()); // Inicia la conexión

    uint32_t started = millis(); // Momento de inicio, para el timeout

    // Espera hasta conectar o agotar el tiempo, animando el LED mientras tanto
    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < CONNECT_TIMEOUT_MS)
    {
        updateStatusLED();
        delay(10);
    }

    // Si no se conectó a tiempo: apaga la radio, marca el fallo y devuelve false
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Saved Wi-Fi connection failed.");
        WiFi.disconnect(true, false); // Apaga la Wi-Fi sin borrar datos
        connectionState = STATE_FAILED;
        return false;
    }

    // Conexión correcta
    connectionState = STATE_CONNECTED;
    connectedAt = millis(); // Anota el momento de conexión

    Serial.println("Connected to saved Wi-Fi.");
    Serial.print("Dashboard IP: ");
    Serial.println(WiFi.localIP());
    return true;
}

// ============================================================================
//  SECCIÓN 10 · SETUP (se ejecuta una vez al arrancar)
// ============================================================================

void setup()
{
    Serial.begin(115200); // Inicia el puerto serie a 115200 baudios
    delay(300);           // Pequeña pausa para estabilizar el serie

    neopixelWrite(RGB_LED_PIN, 0, 0, 0); // Asegura que el LED empieza apagado

    Serial.println();
    Serial.println("ESP32-S3 starting...");

    // Si no se puede conectar con lo guardado, se abre el portal de configuración
    if (!connectSavedWiFi())
        startPortal();

    configureWebServer(); // Registra las rutas (una sola vez)
    server.begin();       // Arranca el servidor HTTP
}

// ============================================================================
//  SECCIÓN 11 · LOOP (se ejecuta continuamente)
// ============================================================================

void loop()
{
    server.handleClient(); // Atiende las peticiones HTTP pendientes
    updateStatusLED();     // Mantiene el LED actualizado

    // ---- Supervisar la conexión iniciada desde el portal ----
    if (connectionState == STATE_CONNECTING)
    {

        if (WiFi.status() == WL_CONNECTED)
        {
            // Éxito: la red y la contraseña son correctas
            connectionState = STATE_CONNECTED;
            connectedAt = millis();

            // Guarda las credenciales en flash solo ahora que se sabe que funcionan
            if (pendingSSID.length() > 0)
            {
                preferences.begin("wifi-config", false); // Modo escritura
                preferences.putString("ssid", pendingSSID);
                preferences.putString("password", pendingPassword);
                preferences.end();
            }

            pendingPassword = ""; // Borra la contraseña de la RAM

            Serial.println("Wi-Fi connected.");
            Serial.print("SSID: ");
            Serial.println(WiFi.SSID());
            Serial.print("Dashboard IP: ");
            Serial.println(WiFi.localIP());
        }
        else if (millis() - connectionStartedAt >= CONNECT_TIMEOUT_MS)
        {
            // Fallo: se agotó el tiempo sin conseguir conexión
            connectionState = STATE_FAILED;
            pendingPassword = ""; // Borra la contraseña de la RAM
            Serial.println("Wi-Fi connection timed out.");
        }
    }

    // ---- Apagar el AP de configuración tras conectar con éxito ----
    // Se espera AP_SHUTDOWN_DELAY_MS para que el navegador alcance a mostrar la IP.
    if (portalActive &&
        connectionState == STATE_CONNECTED &&
        millis() - connectedAt >= AP_SHUTDOWN_DELAY_MS)
    {

        WiFi.softAPdisconnect(true); // Apaga el punto de acceso
        WiFi.mode(WIFI_STA);         // Deja solo el modo cliente
        portalActive = false;        // El portal ya no está activo

        Serial.println("Setup AP disabled.");
        Serial.print("Dashboard remains available at: http://");
        Serial.println(WiFi.localIP());
    }

    delay(2); // Cede tiempo a otras tareas del sistema
}
