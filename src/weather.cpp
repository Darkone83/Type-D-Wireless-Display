#include "weather.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

namespace Weather {

static Preferences prefs;
static Config cfg;
static Snapshot snap;

enum class Phase : uint8_t { IDLE, GEO_ASK, GEO_WAIT, WX_ASK, WX_WAIT };
static Phase phase = Phase::IDLE;

static uint32_t t_lastStep = 0;
static uint32_t t_lastOK   = 0;
static uint32_t t_backoff  = 0;

static const uint32_t GEO_RETRY_MS  = 60 * 1000UL;
static const uint32_t WX_MIN_MS     = 10 * 60 * 1000UL;  // refresh every 10 min
static const uint32_t WX_RETRY_MS   = 30 * 1000UL;       // retry quickly on fail

// -------------- tiny helpers --------------
static inline bool strFindNum(const String& s, const char* key, float& out) {
  int k = s.indexOf(key);
  if (k < 0) return false;
  k += strlen(key);
  // skip separators ':', space
  while (k < (int)s.length() && (s[k]==':' || s[k]==' ')) k++;
  // read up to comma/brace
  int e = k;
  while (e < (int)s.length() && (s[e]=='.' || (s[e]>='0' && s[e]<='9') || s[e]=='-' )) e++;
  out = s.substring(k, e).toFloat();
  return true;
}
static inline bool strFindInt(const String& s, const char* key, int& out) {
  float f;
  if (!strFindNum(s, key, f)) return false;
  out = (int)lroundf(f);
  return true;
}

static String wmoToText(int code) {
  // Minimal WMO → text mapper
  // (0) Clear; (1..3) Mainly clear/partly cloudy/overcast; (45/48) Fog;
  // (51..57) Drizzle; (61..67) Rain; (71..77) Snow; (80..82) Rain showers; (95..99) Thunder
  if (code == 0) return "Clear";
  if (code >= 1 && code <= 3) return "Partly cloudy";
  if (code == 45 || code == 48) return "Fog";
  if ((code >= 51 && code <= 57) || (code >= 61 && code <= 67)) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 95 && code <= 99) return "Thunder";
  return "—";
}

// -------------- persistence --------------
static void saveCfg() {
  prefs.begin("weather", false);
  prefs.putBool("en",  cfg.enabled);
  prefs.putBool("al",  cfg.autoLocate);
  prefs.putDouble("lat", cfg.lat);
  prefs.putDouble("lon", cfg.lon);
  prefs.putBool("f",   cfg.useFahrenheit);
  prefs.putString("key", cfg.apiKey);
  prefs.end();
}
static void loadCfg() {
  prefs.begin("weather", true);
  cfg.enabled       = prefs.getBool("en", true);
  cfg.autoLocate    = prefs.getBool("al", true);
  cfg.lat           = prefs.getDouble("lat", NAN);
  cfg.lon           = prefs.getDouble("lon", NAN);
  cfg.useFahrenheit = prefs.getBool("f", true);
  cfg.apiKey        = prefs.getString("key", "");
  prefs.end();
}

// -------------- HTTP(S) GET --------------
static bool httpsGET(const char* host, const String& path, String& out, uint16_t port = 443) {
  WiFiClientSecure cli;
  cli.setInsecure();                 // keep it simple (no CA)
  cli.setTimeout(6000);
  if (!cli.connect(host, port)) return false;

  String req = String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: TypeD-WD/1.0\r\n" +
               "Connection: close\r\n\r\n";
  if (cli.print(req) == 0) return false;

  // skip headers
  if (!cli.find("\r\n\r\n")) return false;

  out = "";
  char buf[256];
  while (true) {
    int n = cli.read((uint8_t*)buf, sizeof(buf));
    if (n <= 0) break;
    out.concat(String(buf).substring(0, n));
    if (out.length() > 8192) break; // hard cap
  }
  return out.length() > 0;
}

static bool httpGET(const char* host, const String& path, String& out, uint16_t port = 80) {
  WiFiClient cli;
  cli.setTimeout(6000);
  if (!cli.connect(host, port)) return false;

  String req = String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: TypeD-WD/1.0\r\n" +
               "Connection: close\r\n\r\n";
  if (cli.print(req) == 0) return false;

  if (!cli.find("\r\n\r\n")) return false;

  out = "";
  char buf[256];
  while (true) {
    int n = cli.read((uint8_t*)buf, sizeof(buf));
    if (n <= 0) break;
    out.concat(String(buf).substring(0, n));
    if (out.length() > 4096) break;
  }
  return out.length() > 0;
}

// -------------- flow --------------
static void requestGeo() {
  phase = Phase::GEO_ASK;
  t_lastStep = millis();
}
static void requestWx() {
  phase = Phase::WX_ASK;
  t_lastStep = millis();
}

void begin() {
  loadCfg();
  snap = Snapshot();
  phase = Phase::IDLE;
  t_lastOK = 0;
  t_backoff = 0;
}

void setConfig(const Config& c) {
  cfg = c;
  saveCfg();
  // restart cycle immediately
  phase = Phase::IDLE;
  t_lastOK = 0;
  t_backoff = 0;
}

Config getConfig() { return cfg; }

bool isReady() { return snap.ok; }
Snapshot get() { return snap; }

void loop() {
  if (!cfg.enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();

  // Decide what to do next
  if (phase == Phase::IDLE) {
    bool needGeo = cfg.autoLocate && (isnan(cfg.lat) || isnan(cfg.lon));
    bool stale   = (t_lastOK == 0) || (now - t_lastOK >= WX_MIN_MS);
    if (needGeo) {
      requestGeo();
    } else if (stale && now - t_backoff >= 1) {
      requestWx();
    }
    return;
  }

  // GEO query (ip-api)
  if (phase == Phase::GEO_ASK || phase == Phase::GEO_WAIT) {
    String body;
    bool ok = httpGET("ip-api.com", "/json/?fields=status,lat,lon", body);
    if (!ok) { t_backoff = now + GEO_RETRY_MS; phase = Phase::IDLE; return; }

    // Expect: {"status":"success","lat":..,"lon":..}
    int okpos = body.indexOf("\"success\"");
    float lat=0, lon=0;
    bool haveLat = strFindNum(body, "\"lat\":", lat);
    bool haveLon = strFindNum(body, "\"lon\":", lon);

    if (okpos >= 0 && haveLat && haveLon) {
      cfg.lat = lat; cfg.lon = lon;
      saveCfg();
      phase = Phase::IDLE;        // next loop will request weather
      t_backoff = 0;
    } else {
      t_backoff = now + GEO_RETRY_MS;
      phase = Phase::IDLE;
    }
    return;
  }

  // Weather query (Open-Meteo)
  if (phase == Phase::WX_ASK || phase == Phase::WX_WAIT) {
    if (isnan(cfg.lat) || isnan(cfg.lon)) { phase = Phase::IDLE; return; }

    char path[256];
    // unit selection
    const char* unit = cfg.useFahrenheit ? "fahrenheit" : "celsius";
    snprintf(path, sizeof(path),
      "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,relative_humidity_2m"
      "&temperature_unit=%s&forecast_days=1&timeformat=unixtime&timezone=auto",
      cfg.lat, cfg.lon, unit);

    String body;
    bool ok = httpsGET("api.open-meteo.com", String(path), body);
    if (!ok) { t_backoff = now + WX_RETRY_MS; phase = Phase::IDLE; return; }

    // Parse minimal current fields
    float temp=0; int wmo=0, rh=0;
    bool a = strFindNum(body, "\"temperature_2m\":", temp);
    bool b = strFindInt(body, "\"weather_code\":", wmo);
    bool c = strFindInt(body, "\"relative_humidity_2m\":", rh);

    if (a && b) {
      snap.ok = true;
      snap.ts = now;
      snap.tempC = cfg.useFahrenheit ? ((temp - 32.0f) * 5.0f/9.0f) : temp;
      snap.wmo = wmo;
      snap.humidity = c ? rh : -1;
      snap.text = wmoToText(wmo);
      t_lastOK = now;
      t_backoff = now + WX_MIN_MS;
    } else {
      t_backoff = now + WX_RETRY_MS;
    }

    phase = Phase::IDLE;
    return;
  }
}

} // namespace Weather
