#include "wifimgr.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "led_stat.h"
#include <vector>
#include "esp_wifi.h"
#include <math.h>        // isnan()
#include <HTTPClient.h>  // for /weather/autoloc
#include <Update.h>      // OTA

// ===== Server (shared) =====
static AsyncWebServer server(80);

namespace WiFiMgr {

static String ssid, password;
static Preferences prefs;           // used for wifi creds, weather config, and UI/display
static DNSServer dnsServer;
static std::vector<String> lastScanResults;

enum class State { IDLE, CONNECTING, CONNECTED, PORTAL };
static State state = State::PORTAL;

static int connectAttempts = 0;
static const int maxAttempts = 10;
static unsigned long lastAttempt = 0;
static unsigned long retryDelay = 3000;

AsyncWebServer& getServer() { return server; }

// ===== NEW: Display selection (persisted) =====
static String dispMode = "ssd1309";   // "ssd1309" (default) or "us2066"

static void loadDisplayPref() {
  prefs.begin("ui", true);
  dispMode = prefs.getString("display", "ssd1309");
  prefs.end();
  if (dispMode != "us2066") dispMode = "ssd1309"; // clamp to known values
}
static void saveDisplayPref(const String& m) {
  String v = (m == "us2066") ? "us2066" : "ssd1309";
  prefs.begin("ui", false);
  prefs.putString("display", v);
  prefs.end();
  dispMode = v;
}
// Public helpers (declare in .h)
String getDisplay() { return dispMode; }
bool   isUS2066Selected() { return dispMode == "us2066"; }

// ===== OTA HTML (very small) =====
static const char OTA_PAGE[] PROGMEM = R"html(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Type D OTA</title>
<style>
body{background:#111;color:#EEE;font-family:sans-serif;margin:24px}
.card{max-width:360px;margin:auto;background:#1a1a1a;border:1px solid #333;border-radius:10px;padding:14px}
button,input[type=submit]{background:#299a2c;color:#fff;border:0;border-radius:6px;padding:.6em 1em}
input[type=file]{width:100%;margin:.6em 0}
.row{display:flex;gap:.5em}.row>*{flex:1}
.danger{background:#a22}
small{opacity:.75}
a{color:#8acfff}
</style></head><body>
<div class="card">
  <h2>OTA Update</h2>
  <form method="POST" action="/ota" enctype="multipart/form-data" id="f">
    <input type="file" name="firmware" accept=".bin,.bin.gz" required>
    <div class="row">
      <input type="submit" value="Upload & Flash">
      <button type="button" onclick="reboot()" class="danger">Reboot</button>
    </div>
  </form>
  <div id="s"></div>
  <small>Upload a compiled ESP32 firmware (.bin). Device will reboot automatically on success.</small>
  <p><a href="/"><- Back to Setup</a></p>
</div>
<script>
const s=document.getElementById('s');
function reboot(){ fetch('/reboot',{method:'POST'}).then(_=>location.reload()).catch(_=>0); }
document.getElementById('f').addEventListener('submit', e=>{ s.textContent='Uploading...'; });
</script>
</body></html>
)html";

// ===== AP config =====
static void setAPConfig() {
  WiFi.softAPConfig(
      IPAddress(192, 168, 4, 1),
      IPAddress(192, 168, 4, 1),
      IPAddress(255, 255, 255, 0));
}

// ===== WiFi creds =====
void loadCreds() {
  prefs.begin("wifi", true);
  ssid     = prefs.getString("ssid", "");
  password = prefs.getString("pass", "");
  prefs.end();
}

void saveCreds(const String& s, const String& p) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

void clearCreds() {
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

// ===== Weather helpers (Preferences-backed; no ArduinoJson to keep binary small) =====
static void loadWeatherJSON(String& out) {
  prefs.begin("weather", true);
  bool   enabled = prefs.getBool("enabled", false);
  String units   = prefs.getString("units", "F");     // "F" or "C"
  double lat     = prefs.getDouble("lat", NAN);
  double lon     = prefs.getDouble("lon", NAN);
  int    refresh = prefs.getInt("refresh", 10);       // minutes
  String name    = prefs.getString("name", "");       // label to show
  prefs.end();

  out.reserve(160);
  out = "{";
  out += "\"enabled\":"; out += (enabled ? "true":"false"); out += ",";
  out += "\"units\":\""; out += units; out += "\",";            // "F"|"C"
  out += "\"lat\":"; out += (isnan(lat) ? String("null") : String(lat, 6)); out += ",";
  out += "\"lon\":"; out += (isnan(lon) ? String("null") : String(lon, 6)); out += ",";
  out += "\"refresh\":"; out += refresh; out += ",";
  out += "\"name\":\""; out += name; out += "\"";
  out += "}";
}

// ===== OTA route registration =====
static void registerOTARoutes() {
  // Version endpoint (optional)
  server.on("/fw", HTTP_GET, [](AsyncWebServerRequest* req){
    String v = String("TypeD/") + String(__DATE__) + " " + String(__TIME__);
    req->send(200, "text/plain", v);
  });

  // OTA page
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send_P(200, "text/html", OTA_PAGE);
  });

  // OTA upload/flash
  server.on(
    "/ota",
    HTTP_POST,
    // When upload finishes:
    [](AsyncWebServerRequest* request){
      bool ok = !Update.hasError();
      if (ok) {
        request->send(200, "text/plain", "Update successful, rebooting...");
        Serial.println("[OTA] Update successful, rebooting...");
        LedStat::setStatus(LedStatus::Booting);
        delay(500);
        ESP.restart();
      } else {
        request->send(500, "text/plain", "Update failed");
        Serial.println("[OTA] Update failed");
      }
    },
    // Upload handler (stream chunks to flash)
    [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final){
      if (index == 0) {
        Serial.printf("[OTA] Starting update: %s\n", filename.c_str());
        LedStat::setStatus(LedStatus::Booting);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      if (len) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }
      if (final) {
        if (!Update.end(true)) {
          Update.printError(Serial);
        } else {
          Serial.printf("[OTA] Finished: %u bytes\n", (unsigned)(index + len));
        }
      }
    }
  );

  // Reboot helper
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* req){
    req->send(200, "text/plain", "Rebooting...");
    Serial.println("[OTA] Reboot requested");
    LedStat::setStatus(LedStatus::Booting);
    delay(300);
    ESP.restart();
  });
  // Optional GET for convenience
  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "text/plain", "Rebooting...");
    Serial.println("[OTA] Reboot requested (GET)");
    LedStat::setStatus(LedStatus::Booting);
    delay(300);
    ESP.restart();
  });
}

// ===== Portal =====
void startPortal() {
  WiFi.disconnect(true);
  delay(100);
  setAPConfig();
  WiFi.mode(WIFI_AP_STA);  // AP+STA (ESP32-S3 friendly)
  delay(100);

  // Use a standard channel for best compatibility
  bool apok = WiFi.softAP("Type D Wireless Display Setup", "", 6, 0);
  esp_wifi_set_max_tx_power(20);
  LedStat::setStatus(LedStatus::Portal);
  Serial.printf("[WiFiMgr] softAP result: %d, IP: %s\n", apok, WiFi.softAPIP().toString().c_str());
  delay(200);

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);

  server.reset(); // avoid duplicate routes on re-entry

  // --- OTA routes (available in both AP and STA modes) ---
  registerOTARoutes();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Type D Wireless Display Setup</title>
  <meta name="viewport" content="width=360,initial-scale=1">
  <style>
    body{background:#111;color:#EEE;font-family:sans-serif}
    .container{max-width:360px;margin:24px auto;background:#222;padding:1.2em 1.4em;border-radius:10px;box-shadow:0 0 16px #0008}
    h2{margin:.2em 0 .6em 0}
    input,select,button{width:100%;box-sizing:border-box;margin:.5em 0;padding:.55em;font-size:1.05em;border-radius:6px;border:1px solid #555;background:#111;color:#EEE}
    .row{display:flex;gap:.5em}
    .row > *{flex:1}
    .btn-primary{background:#299a2c;color:white}
    .btn-danger{background:#a22;color:white}
    .card{background:#1a1a1a;border:1px solid #333;border-radius:10px;padding:10px;margin-top:14px}
    .inline{display:flex;align-items:center;gap:.4em}
    .status{margin-top:.6em;font-size:.95em}
    small{opacity:.75}
    a{color:#8acfff}
  </style>
</head>
<body>
  <div class="container">
    <h2>Type D Wireless Display Setup</h2>
    <div class="card">
      <label>Wi-Fi Network</label>
      <select id="ssidDropdown"><option value="">Please select a network</option></select>
      <input type="text" id="ssid" placeholder="SSID">
      <label>Password</label>
      <input type="password" id="pass" placeholder="Wi-Fi Password">
      <button type="button" onclick="saveWifi()" class="btn-primary">Connect & Save</button>
      <button type="button" onclick="forget()" class="btn-danger">Forget Wi-Fi</button>
      <div class="status" id="status">Status: ...</div>
      <small><a href="/ota">OTA Update</a> -=- <a href="/fw">Firmware info</a></small>
    </div>

    <h2>Weather</h2>
    <div class="card">
      <label class="inline"><input type="checkbox" id="w_enabled"> Enable weather screen</label>
      <div class="row">
        <select id="w_units">
          <option value="F">Units: Fahrenheit (°F)</option>
          <option value="C">Units: Celsius (°C)</option>
        </select>
        <input type="number" id="w_refresh" min="1" max="120" value="10" step="1" placeholder="Refresh (min)">
      </div>
      <input type="text" id="w_name" placeholder="Location name (optional)">
      <div class="row">
        <input type="text" id="w_lat" placeholder="Latitude e.g. 40.7128">
        <input type="text" id="w_lon" placeholder="Longitude e.g. -74.0060">
      </div>
      <div class="row">
        <button type="button" onclick="autoLoc()">Auto-detect by IP</button>
        <button type="button" onclick="saveWeather()" class="btn-primary">Save Weather</button>
      </div>
      <div id="w_status" class="status"></div>
      <small>We use Open-Meteo (no API key). Auto-detect uses ip-api.com.</small>
    </div>

    <h2>Display</h2>
    <div class="card">
      <label for="d_mode">Active Display</label>
      <select id="d_mode">
        <option value="ssd1309">OLED 128x64 (SSD1309) — default</option>
        <option value="us2066">Character OLED 20x4 (US2066)</option>
      </select>
      <small>Only one display can be active. Weather is skipped on US2066.</small>
      <div class="row">
        <button type="button" onclick="saveDisplay()" class="btn-primary">Save Display</button>
      </div>
    </div>
  </div>

<script>
function scan() {
  fetch('/scan').then(r=>r.json()).then(list=>{
    let dd=document.getElementById('ssidDropdown'); dd.innerHTML='';
    let def=document.createElement('option'); def.value=''; def.text='Please select a network'; dd.appendChild(def);
    list.forEach(s=>{ let o=document.createElement('option'); o.value=s; o.text=s; dd.appendChild(o); });
    dd.onchange=function(){ document.getElementById('ssid').value=dd.value; };
  }).catch(()=>{
    let dd=document.getElementById('ssidDropdown'); dd.innerHTML='';
    let o=document.createElement('option'); o.value=''; o.text='Scan failed'; dd.appendChild(o);
  });
}
setInterval(scan, 2000); window.onload = ()=>{ scan(); loadWeather(); loadDisplay(); };

function saveWifi(){
  let ssid=document.getElementById('ssid').value;
  let pass=document.getElementById('pass').value;
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,pass:pass})})
      .then(r=>r.text()).then(t=>{ document.getElementById('status').innerText=t; });
}
function forget(){
  fetch('/forget').then(r=>r.text()).then(t=>{
    document.getElementById('status').innerText=t;
    document.getElementById('ssid').value='';
    document.getElementById('pass').value='';
  });
}

function loadWeather(){
  fetch('/weather/get').then(r=>r.json()).then(j=>{
    document.getElementById('w_enabled').checked = !!j.enabled;
    document.getElementById('w_units').value = j.units || 'F';
    document.getElementById('w_refresh').value = j.refresh || 10;
    document.getElementById('w_name').value = j.name || '';
    document.getElementById('w_lat').value = (j.lat==null?'':j.lat);
    document.getElementById('w_lon').value = (j.lon==null?'':j.lon);
  }).catch(()=>{});
}
function saveWeather(){
  let payload = {
    enabled: document.getElementById('w_enabled').checked,
    units: document.getElementById('w_units').value,
    refresh: parseInt(document.getElementById('w_refresh').value||'10',10),
    name: document.getElementById('w_name').value||'',
    lat: parseFloat(document.getElementById('w_lat').value),
    lon: parseFloat(document.getElementById('w_lon').value)
  };
  fetch('/weather/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(r=>r.text()).then(t=>{ document.getElementById('w_status').innerText=t; });
}
function autoLoc(){
  document.getElementById('w_status').innerText='Detecting...';
  fetch('/weather/autoloc').then(r=>r.json()).then(j=>{
    if(j.ok){
      if(j.lat!=null) document.getElementById('w_lat').value=j.lat;
      if(j.lon!=null) document.getElementById('w_lon').value=j.lon;
      if(j.name){ document.getElementById('w_name').value=j.name; }
      document.getElementById('w_status').innerText='Detected.';
    }else{
      document.getElementById('w_status').innerText='Auto-detect failed.';
    }
  }).catch(()=>{ document.getElementById('w_status').innerText='Auto-detect failed.'; });
}

// === Display mode helpers ===
function loadDisplay(){
  fetch('/display/get').then(r=>r.json()).then(j=>{
    document.getElementById('d_mode').value = j.display || 'ssd1309';
  }).catch(()=>{});
}
function saveDisplay(){
  let v = document.getElementById('d_mode').value || 'ssd1309';
  fetch('/display/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({display:v})})
    .then(r=>r.text()).then(t=>alert(t)).catch(()=>{});
}
</script>
</body></html>
    )rawliteral";
    request->send(200, "text/html", page);
  });

  // ===== Weather endpoints =====

  // GET current config as JSON
  server.on("/weather/get", HTTP_GET, [](AsyncWebServerRequest* req){
    String json; loadWeatherJSON(json);
    req->send(200, "application/json", json);
  });

  // POST save config (naive JSON parse to avoid ArduinoJson dep)
  server.on("/weather/save", HTTP_POST,
    [](AsyncWebServerRequest* req){},
    NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      String body; body.reserve(len);
      for (size_t i=0;i<len;i++) body += (char)data[i];

      auto findStr=[&](const char* k)->String{
        int ks=body.indexOf(String("\"")+k+"\":\""); if(ks<0) return "";
        ks += strlen(k)+4; int ke=body.indexOf("\"", ks); if(ke<0) return "";
        return body.substring(ks, ke);
      };
      auto findNum=[&](const char* k, bool allowFloat)->double{
        int ks=body.indexOf(String("\"")+k+"\":"); if(ks<0) return NAN;
        ks += strlen(k)+3; int ke=ks;
        while (ke<(int)body.length() && String("0123456789+-.").indexOf(body[ke])>=0) ke++;
        return allowFloat ? body.substring(ks, ke).toDouble() : body.substring(ks, ke).toInt();
      };

      bool   enabled = body.indexOf("\"enabled\":true")>=0;
      String units   = findStr("units"); if (units!="C") units="F";
      int    refresh = (int)findNum("refresh", false); if (refresh<=0) refresh=10;
      double lat     = findNum("lat", true);
      double lon     = findNum("lon", true);
      String name    = findStr("name");

      prefs.begin("weather", false);
      prefs.putBool("enabled", enabled);
      prefs.putString("units", units);
      prefs.putInt("refresh", refresh);
      if (!isnan(lat)) prefs.putDouble("lat", lat);
      if (!isnan(lon)) prefs.putDouble("lon", lon);
      prefs.putString("name", name);
      prefs.end();

      req->send(200, "text/plain", "Weather settings saved.");
    }
  );

  // GET geolocate via IP (ip-api.com), no key required
  server.on("/weather/autoloc", HTTP_GET, [](AsyncWebServerRequest* req){
    if (WiFi.status() != WL_CONNECTED) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"wifi\"}");
      return;
    }
    HTTPClient http;
    http.setTimeout(4000);
    if (!http.begin("http://ip-api.com/json")) {
      req->send(200, "application/json", "{\"ok\":false}");
      return;
    }
    int code = http.GET();
    if (code!=200) { http.end(); req->send(200, "application/json", "{\"ok\":false}"); return; }
    String body = http.getString(); http.end();

    // crude extract lat/lon/city/region
    auto pickStr=[&](const char* key)->String{
      String k=String("\"")+key+"\":\""; int s=body.indexOf(k); if(s<0) return "";
      s += k.length(); int e=body.indexOf("\"", s); if(e<0) return ""; return body.substring(s,e);
    };
    auto pickNum=[&](const char* key)->String{
      String k=String("\"")+key+"\":";
      int s=body.indexOf(k); if(s<0) return "";
      s += k.length(); int e=s;
      while (e<(int)body.length() && String("0123456789+-.").indexOf(body[e])>=0) e++;
      return body.substring(s,e);
    };

    String lat = pickNum("lat"), lon = pickNum("lon");
    String city = pickStr("city"), region = pickStr("regionName");
    String name = city.length()? (region.length()? city+", "+region:city) : "";

    String out = "{\"ok\":";
    out += (lat.length() && lon.length() ? "true" : "false");
    if (lat.length())  { out += ",\"lat\":"+lat; }
    if (lon.length())  { out += ",\"lon\":"+lon; }
    if (name.length()) { out += ",\"name\":\""+name+"\""; }
    out += "}";

    req->send(200, "application/json", out);
  });

  // ===== NEW: Display mode endpoints =====
  server.on("/display/get", HTTP_GET, [](AsyncWebServerRequest* req){
    String json = "{\"display\":\"" + dispMode + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/display/save", HTTP_POST,
    [](AsyncWebServerRequest* req){},
    NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      String body; body.reserve(len);
      for (size_t i=0;i<len;i++) body += (char)data[i];

      // Very small parse: {"display":"us2066"} or "ssd1309"
      int ks = body.indexOf("\"display\":\"");
      String v = "ssd1309";
      if (ks >= 0) {
        ks += 11; // after "display\":\""
        int ke = body.indexOf("\"", ks);
        if (ke > ks) v = body.substring(ks, ke);
      }
      saveDisplayPref(v);
      req->send(200, "text/plain", "Display saved: " + dispMode);
    }
  );

  // ===== Existing endpoints =====

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String stat;
    if (WiFi.status() == WL_CONNECTED)
      stat = "Connected to " + WiFi.SSID() + " - IP: " + WiFi.localIP().toString();
    else if (state == State::CONNECTING)
      stat = "Connecting to " + ssid + "...";
    else
      stat = "In portal mode";
    request->send(200, "text/plain", stat);
  });

  server.on("/connect", HTTP_GET, [](AsyncWebServerRequest *request){
    String ss, pw;
    if (request->hasParam("ssid")) ss = request->getParam("ssid")->value();
    if (request->hasParam("pass")) pw = request->getParam("pass")->value();
    if (ss.length() == 0) { request->send(400, "text/plain", "SSID missing"); return; }
    saveCreds(ss, pw);
    ssid = ss;
    password = pw;
    state = State::CONNECTING;
    connectAttempts = 1;
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
    request->send(200, "text/plain", "Connecting to: " + ssid);
  });

  // cached scan endpoint for reliability
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    int n = WiFi.scanComplete();
    if (n == -2) {
      WiFi.scanNetworks(true, true);
      String json = "[";
      for (size_t i = 0; i < lastScanResults.size(); ++i) {
        if (i) json += ",";
        json += "\"" + lastScanResults[i] + "\"";
      }
      json += "]";
      request->send(200, "application/json", json);
      return;
    } else if (n == -1) {
      String json = "[";
      for (size_t i = 0; i < lastScanResults.size(); ++i) {
        if (i) json += ",";
        json += "\"" + lastScanResults[i] + "\"";
      }
      json += "]";
      request->send(200, "application/json", json);
      return;
    }
    lastScanResults.clear();
    for (int i = 0; i < n; ++i) lastScanResults.push_back(WiFi.SSID(i));
    WiFi.scanDelete();
    String json = "[";
    for (size_t i = 0; i < lastScanResults.size(); ++i) {
      if (i) json += ",";
      json += "\"" + lastScanResults[i] + "\"";
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  server.on("/forget", HTTP_GET, [](AsyncWebServerRequest *request){
    clearCreds();
    ssid = ""; password = "";
    WiFi.disconnect();
    state = State::PORTAL;
    request->send(200, "text/plain", "WiFi credentials cleared.");
  });

  server.on("/debug/forget", HTTP_GET, [](AsyncWebServerRequest *request){
    clearCreds();
    ssid = ""; password = "";
    WiFi.disconnect(true);
    state = State::PORTAL;
    Serial.println("[DEBUG] WiFi credentials cleared via /debug/forget");
    request->send(200, "text/plain", "WiFi credentials cleared (debug).");
  });

  // Proper POST JSON body for ESPAsyncWebServer
  server.on("/save", HTTP_POST,
    [](AsyncWebServerRequest *request){},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
      String body = "";
      body.reserve(len);
      for (size_t i = 0; i < len; i++) body += (char)data[i];
      // crude parse: {"ssid":"...","pass":"..."}
      int ssidStart = body.indexOf("\"ssid\":\"") + 8;
      int ssidEnd   = body.indexOf("\"", ssidStart);
      int passStart = body.indexOf("\"pass\":\"") + 8;
      int passEnd   = body.indexOf("\"", passStart);
      String newSsid = (ssidStart >= 8 && ssidEnd > ssidStart) ? body.substring(ssidStart, ssidEnd) : "";
      String newPass = (passStart >= 8 && passEnd > passStart) ? body.substring(passStart, passEnd) : "";
      if (newSsid.length() == 0) { request->send(400, "text/plain", "SSID missing"); return; }
      saveCreds(newSsid, newPass);
      ssid = newSsid;
      password = newPass;
      state = State::CONNECTING;
      connectAttempts = 1;
      WiFi.begin(newSsid.c_str(), newPass.c_str());
      request->send(200, "text/plain", "Connecting to: " + newSsid);
      Serial.printf("[WiFiMgr] Received new creds. SSID: %s\n", newSsid.c_str());
    }
  );

  auto cp = [](AsyncWebServerRequest *r){
    r->send(200, "text/html", "<meta http-equiv='refresh' content='0; url=/' />");
  };
  server.on("/generate_204", HTTP_GET, cp);
  server.on("/hotspot-detect.html", HTTP_GET, cp);
  server.on("/redirect", HTTP_GET, cp);
  server.on("/ncsi.txt", HTTP_GET, cp);
  server.on("/captiveportal", HTTP_GET, cp);
  server.onNotFound(cp);

  server.begin();
  state = State::PORTAL;
}

void stopPortal() { dnsServer.stop(); }

void tryConnect() {
  if (ssid.length() > 0) {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
    state = State::CONNECTING;
    connectAttempts = 1;
    lastAttempt = millis();
  } else {
    startPortal();
  }
}

void begin() {
  LedStat::setStatus(LedStatus::Booting);
  loadCreds();
  loadDisplayPref(); // NEW: load persisted display selection
  startPortal();
  if (ssid.length() > 0) tryConnect();
}

void loop() {
  dnsServer.processNextRequest();
  if (state == State::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      state = State::CONNECTED;
      dnsServer.stop();
      Serial.println("[WiFiMgr] WiFi connected.");
      Serial.print("[WiFiMgr] IP Address: ");
      Serial.println(WiFi.localIP());
      LedStat::setStatus(LedStatus::WifiConnected);
    } else if (millis() - lastAttempt > retryDelay) {
      connectAttempts++;
      if (connectAttempts >= maxAttempts) {
        state = State::PORTAL;
        startPortal();
        LedStat::setStatus(LedStatus::WifiFailed);
      } else {
        WiFi.disconnect();
        WiFi.begin(ssid.c_str(), password.c_str());
        lastAttempt = millis();
      }
    }
  }
}

void restartPortal() { startPortal(); }

void forgetWiFi() {
  clearCreds();
  startPortal();
}

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

String getStatus() {
  if (isConnected()) return "Connected to: " + ssid;
  if (state == State::CONNECTING) return "Connecting to: " + ssid;
  return "Not connected";
}

} // namespace WiFiMgr
