// display.cpp — Type-D Wireless Display (SSD1309) with ConsoleMods-based Xbox version guessing
// Screens: MAIN / SECOND / HEALTH / WEATHER
// - MAIN: rows + scrolling game quote; new quote on MAIN entry & every 60s
//         + Battery widget on the Fan row (right-justified) if LC709203F is detected
// - SECOND: dense info (Tray/AV/Xbox/Encoder/Serial/MAC/Region)
// - HEALTH: simplified, text-only (WiFi RSSI, Free memory, IP)
// - WEATHER: icon + temp + cond, humidity, wind (Open-Meteo, keyless)
// Inactivity policy (final):
//   - Show ONLY the DC logo (full-screen) before first MAIN packet.
//   - If NO packets (A/B/C) for 2 min -> show DC logo (searching).
//   - If NO packets (A/B/C) for 5 min -> screensaver (bouncing “Sleeping...”).
//   - On any packet -> exit saver and resume.
//   - We NEVER draw “STALE”.
// Transitions: slide-in left/right only.

#include "display.h"
#include <U8g2lib.h>
#include "udp_typed.h"
#include <WiFi.h>
#include <esp_system.h>
#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <math.h>          // isnan(), fabsf()
#include <Preferences.h>

// --- Boot glyph (dc_logo.h) ---
#include "dc_logo.h"

// --- Fuel gauge (Adafruit LC709203F) ---
#include <Adafruit_LC709203F.h>

// --- Weather fetch (Open-Meteo) ---
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace TypeDDisplay {

// ===== Module state =====
static U8G2* g = nullptr;
static bool g_dbg = false;

// Hold times
static uint32_t HOLD_MAIN_MS    = 15000;
static uint32_t HOLD_SECOND_MS  =  5000;
static uint32_t HOLD_HEALTH_MS  =  5000;
static uint32_t HOLD_WEATHER_MS =  7000;   // NEW: weather screen hold

// Draw cadence
static const  uint32_t DRAW_INTERVAL_MS = 200;
static        uint32_t lastDraw = 0;

// Screens
enum class Screen : uint8_t { WAITING=0, MAIN, SECOND, HEALTH, WEATHER };
static Screen   cur = Screen::WAITING;
static uint32_t nextSwitchAt = 0;

// Inactivity thresholds (ANY UDP)
static const uint32_t NO_PACKET_LOGO_MS  = 120000UL;
static const uint32_t NO_PACKET_SAVER_MS = 300000UL;

// Track what we’ve ever seen
static bool     everAnyPacket = false;
static bool     haveMain = false;
// Proper timestamp of last packet arrival (fixes false “Sleeping...”)
static uint32_t lastAnyAt = 0;

// Transitions
enum class Xition : uint8_t { NONE=0, SLIDE_IN_RIGHT, SLIDE_IN_LEFT };
static Xition nextXition = Xition::NONE;

// ===== Data caches from UDP =====
static struct {
  bool have = false;
  int32_t fan = 0;
  int32_t cpu_c = 0;
  int32_t amb_c = 0;
  char app[33] = {0};
} MAIN;

static struct {
  bool have = false;
  int32_t tray = -1;
  int32_t av   = -1;
  int32_t xboxver = -1;
  int32_t enc  = -1;
  int32_t width = 0, height = 0;
} EXT;

static struct {
  bool have = false;
  String serial;
  String mac;
  String region;
} EE;

// ===== Fuel gauge (LC709203F) =====
static Adafruit_LC709203F lc;
static bool lc_ok = false;
static bool lc_tried = false;
static uint32_t lc_lastTry = 0;
static const uint32_t LC_RETRY_MS = 5000;

// Shown on-screen (smoothed)
static float lc_pct = NAN;
static float lc_v   = NAN;
static uint32_t lc_lastRead = 0;
static const uint32_t LC_READ_MS = 3000;

// Accuracy/consistency helpers
static uint32_t lc_ok_since = 0;            // when gauge first came up
static const uint32_t LC_WARMUP_MS   = 1500; // initial settle window
static int lc_warmup_reads           = 0;    // also require N good reads
static const int LC_WARMUP_READS     = 3;

static const float LC_ALPHA_P        = 0.25f; // EMA factor for percent smoothing
static const float LC_ALPHA_V        = 0.30f; // EMA factor for voltage smoothing
static const float LC_MAX_STEP_P     = 6.0f;  // clamp % step per sample
static const float LC_MAX_JUMP_V     = 0.12f; // reject raw V jumps >120 mV/read
static const float LC_MAX_JUMP_P     = 12.0f; // reject raw % jumps >12%/read
static const float LC_BLEND_W        = 0.60f; // weight for library % vs OCV % (0..1)
static float lc_v_filt = NAN;                // filtered voltage
static float last_p_raw = NAN, last_v_raw = NAN;

// Voltage→% (very rough LiPo curve, linear between points)
static float pctFromVoltage(float v) {
  if (v <= 3.30f) return 0.f;
  if (v >= 4.20f) return 100.f;
  struct P { float v, p; };
  const P pts[] = { {4.20,100},{4.08,95},{3.98,90},{3.92,85},{3.86,80},{3.80,75},{3.75,70},{3.70,60},{3.65,50},{3.60,45},{3.55,35},{3.50,25},{3.45,15},{3.40,8},{3.30,0} };
  for (size_t i=1;i<sizeof(pts)/sizeof(pts[0]);++i) if (v >= pts[i].v) {
    const float t = (v - pts[i].v) / (pts[i-1].v - pts[i].v);
    return pts[i].p + t * (pts[i-1].p - pts[i].p);
  }
  return 50.f; // shouldn't reach
}

static inline bool plausibleV(float v) { return (!isnan(v) && v > 3.0f && v < 5.5f); }
static inline bool plausibleP(float p) { return (!isnan(p) && p >= 0.f  && p <= 100.f); }

static void resetGaugeFilters() {
  lc_pct = NAN;
  lc_v = lc_v_filt = NAN;
  last_p_raw = last_v_raw = NAN;
  lc_warmup_reads = 0;
}

// NOTE: We do NOT use a thermistor. No thermistor config calls are made.
static void maybeInitGauge() {
  if (lc_ok) return;
  uint32_t now = millis();
  if (lc_tried && (now - lc_lastTry < LC_RETRY_MS)) return;

  lc_tried = true;
  lc_lastTry = now;

  // Uses default address 0x0B on the existing Wire bus
  if (lc.begin()) {
    lc_ok = true;
    lc_ok_since = millis();
    resetGaugeFilters();

    // If you define a known pack size at build-time, it will be applied.
    #if defined(LC709203F_APA_2200MAH)
      lc.setPackSize(LC709203F_APA_2200MAH);
    #elif defined(LC709203F_APA_2000MAH)
      lc.setPackSize(LC709203F_APA_2000MAH);
    #elif defined(LC709203F_APA_2500MAH)
      lc.setPackSize(LC709203F_APA_2500MAH);
    #endif

    // Keep default temperature behavior (internal IC method). No thermistor used.

    #ifdef LC709203F_POWER_OPERATE
      lc.setPowerMode(LC709203F_POWER_OPERATE);
    #endif
    if (g_dbg) Serial.println("[DISPLAY] LC709203F detected + configured (no thermistor)");
  } else {
    lc_ok = false;
    if (g_dbg) Serial.println("[DISPLAY] LC709203F not found (will retry)");
  }
}

static void updateGaugeReading() {
  if (!lc_ok) return;
  uint32_t now = millis();
  if (now - lc_lastRead < LC_READ_MS) return;
  // give the IC a moment after power-up to stabilize OCV estimate
  if (lc_ok_since && (now - lc_ok_since < LC_WARMUP_MS)) return;
  lc_lastRead = now;

  float p_raw = lc.cellPercent();  // library estimate
  float v_raw = lc.cellVoltage();  // measured voltage

  // Basic plausibility
  if (!plausibleV(v_raw)) v_raw = NAN;
  if (!plausibleP(p_raw)) p_raw = NAN;

  // Reject sudden jumps (vs last raw)
  if (plausibleV(v_raw) && plausibleV(last_v_raw)) {
    if (fabsf(v_raw - last_v_raw) > LC_MAX_JUMP_V) v_raw = NAN;
  }
  if (plausibleP(p_raw) && plausibleP(last_p_raw)) {
    if (fabsf(p_raw - last_p_raw) > LC_MAX_JUMP_P) p_raw = NAN;
  }

  // Update last_raw if valid
  if (!isnan(v_raw)) last_v_raw = v_raw;
  if (!isnan(p_raw)) last_p_raw = p_raw;

  // Voltage EMA
  if (!isnan(v_raw)) {
    if (isnan(lc_v_filt)) lc_v_filt = v_raw;
    else lc_v_filt += LC_ALPHA_V * (v_raw - lc_v_filt);
    lc_v = lc_v_filt; // expose filtered V for UI/debug
  }

  // Warm-up: require a few valid samples total before showing %
  if (lc_warmup_reads < LC_WARMUP_READS) {
    if (!isnan(v_raw) || !isnan(p_raw)) lc_warmup_reads++;
    if (lc_warmup_reads < LC_WARMUP_READS) return;
  }

  // Stable % estimate:
  // - Prefer library percent when sane
  // - Blend with OCV-derived percent from filtered voltage
  float p_est = NAN;
  const bool have_lib = plausibleP(p_raw);
  const bool have_v   = plausibleV(lc_v_filt);
  float p_ocv = have_v ? pctFromVoltage(lc_v_filt) : NAN;

  if (have_lib && have_v)      p_est = LC_BLEND_W * p_raw + (1.f - LC_BLEND_W) * p_ocv;
  else if (have_lib)           p_est = p_raw;
  else if (have_v)             p_est = p_ocv;

  // Smooth + clamp steps + monotonic guard
  if (!isnan(p_est)) {
    if (isnan(lc_pct)) {
      lc_pct = p_est;
    } else {
      float delta = p_est - lc_pct;

      // Monotonic guard: if voltage clearly falling (>30 mV step), don’t let % rise
      if (!isnan(last_v_raw) && !isnan(lc_v_filt) && (last_v_raw - lc_v_filt) > 0.03f && delta > 0) {
        delta = 0.f;
      }

      if (delta >  LC_MAX_STEP_P) delta =  LC_MAX_STEP_P;
      if (delta < -LC_MAX_STEP_P) delta = -LC_MAX_STEP_P;
      lc_pct += LC_ALPHA_P * delta;
    }
    // keep inside [0,100]
    if (lc_pct < 0.f)   lc_pct = 0.f;
    if (lc_pct > 100.f) lc_pct = 100.f;
  }

  if (g_dbg) {
    if (!isnan(lc_pct) && !isnan(lc_v_filt))
      Serial.printf("[DISPLAY] batt=%.1f%% (Vf=%.3f V)%s%s\n",
                    lc_pct, lc_v_filt,
                    have_lib ? "" : " [no-lib%]",
                    (have_v && have_lib ? " [blend]" : ""));
    else if (!isnan(lc_v_filt))
      Serial.printf("[DISPLAY] batt=--%% (Vf=%.3f V)\n", lc_v_filt);
    else
      Serial.printf("[DISPLAY] batt=-- (no gauge)\n");
  }
}

// ===== WEATHER =====
static struct {
  bool   enabled = false;  // portal toggle
  char   units   = 'F';    // 'F' or 'C'
  double lat     = NAN;    // from portal or IP geolocate (stored in Preferences by portal)
  double lon     = NAN;
  uint32_t refresh_ms = 10UL * 60UL * 1000UL; // default 10 min

  bool   ok    = false;    // we have valid data to show
  uint32_t last_fetch = 0;

  // data
  float  temp = NAN;     // degC/degF depending on units
  int    rh   = -1;      // %
  float  wind = NAN;     // mph/kph approx
  int    code = -1;      // weather_code
  char   place[32] = {0}; // optional name (from prefs), else coords
} W;

// (load prefs) — read once at begin(); portal writes them into "weather" namespace
static void loadWeatherPrefs() {
  Preferences prefs;
  if (!prefs.begin("weather", true)) return;

  W.enabled = prefs.getBool("enabled", false);
  String u = prefs.getString("units", "F");
  W.units = (u.length() && (u[0]=='C' || u[0]=='c')) ? 'C' : 'F';
  W.lat = prefs.getDouble("lat", NAN);
  W.lon = prefs.getDouble("lon", NAN);
  int refMin = prefs.getInt("refresh", 10);
  if (refMin < 1) refMin = 1; if (refMin > 120) refMin = 120;
  W.refresh_ms = (uint32_t)refMin * 60UL * 1000UL;
  String nm = prefs.getString("name", "");
  nm.toCharArray(W.place, sizeof(W.place));
  prefs.end();

  if (g_dbg) {
    Serial.printf("[WEATHER] enabled=%d units=%c lat=%.5f lon=%.5f refresh=%lus name='%s'\n",
                  (int)W.enabled, W.units, W.lat, W.lon, W.refresh_ms/1000UL, W.place);
  }
}


// choose biggest temp font that fits in maxW
static const uint8_t* pickTempFont(const char* text, int maxW) {
  const uint8_t* candidates[] = {
    u8g2_font_logisoso24_tf,
    u8g2_font_logisoso20_tf,
    u8g2_font_logisoso16_tf
  };
  for (auto f : candidates) {
    g->setFont(f);
    if (g->getStrWidth(text) <= maxW) return f;
  }
  return u8g2_font_logisoso16_tf;
}

// choose best header font that fits in maxW
static const uint8_t* pickHeaderFont(const String& s, int maxW) {
  g->setFont(u8g2_font_6x12_tf);
  if (g->getStrWidth(s.c_str()) <= maxW) return u8g2_font_6x12_tf;
  return u8g2_font_5x8_tf;
}

static const char* labelForCode(int code) {
  if (code == 0) return "Clear";
  if (code==1) return "Mostly clear";
  if (code==2) return "Partly cloudy";
  if (code==3) return "Overcast";
  if (code==45 || code==48) return "Fog";
  if (code==51 || code==53 || code==55) return "Drizzle";
  if (code==56 || code==57) return "Frz drizzle";
  if (code==61 || code==63 || code==65) return "Rain";
  if (code==66 || code==67) return "Frz rain";
  if (code==71 || code==73 || code==75) return "Snow";
  if (code==77) return "Snow grains";
  if (code==80 || code==81 || code==82) return "Showers";
  if (code==85 || code==86) return "Snow shwrs";
  if (code==95) return "Thunder";
  if (code==96 || code==99) return "T-storm hail";
  return "—";
}

static bool fetchWeatherNow() {
  if (!W.enabled) return false;
  if (!WiFi.isConnected()) return false;
  if (isnan(W.lat) || isnan(W.lon)) return false;

  // Build Open-Meteo URL
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(W.lat, 5) +
               "&longitude=" + String(W.lon, 5) +
               "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m";
  if (W.units == 'F') {
    url += "&temperature_unit=fahrenheit&windspeed_unit=mph";
  } else {
    url += "&temperature_unit=celsius&windspeed_unit=kmh";
  }

  WiFiClientSecure ssl;
  ssl.setInsecure();       // keep it simple; you can pin CA later
  HTTPClient http;
  http.setTimeout(4000);

  if (!http.begin(ssl, url)) {
    if (g_dbg) Serial.println("[WEATHER] http.begin failed");
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    if (g_dbg) Serial.printf("[WEATHER] GET failed: %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  // Parse small JSON
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (g_dbg) Serial.printf("[WEATHER] JSON error: %s\n", err.c_str());
    return false;
  }
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;

  W.temp = cur["temperature_2m"] | NAN;
  W.rh   = cur["relative_humidity_2m"] | -1;
  W.wind = cur["wind_speed_10m"] | NAN;
  W.code = cur["weather_code"] | -1;
  W.ok   = !isnan(W.temp) && (W.code >= 0);
  W.last_fetch = millis();

  if (g_dbg) {
    Serial.printf("[WEATHER] t=%.1f %c rh=%d%% wind=%.1f code=%d\n",
                  W.temp, W.units, W.rh, W.wind, W.code);
  }
  return W.ok;
}

// ===== Quotes =====
static const char* kQuotes[] = {
  "I don't need to get a life. I'm a gamer - I have lots of lives.",
  "I went outside once. The graphics weren't that good.",
  "I paused my game to be here.",
  "Everything is under control.",
  "Eat. Sleep. Game. Repeat.",
  "It's-a me, Mario!",
  "Escape reality and play games.",
  "Tips on how to talk to someone when they're gaming: don't.",
  "When life gets hard, it means you just leveled up.",
  "I can't hear you. I'm gaming.",
  "Not now. I'm saving the world.",
  "Reality is broken but game designers can fix it.",
  "Life is a video game. You always get zapped in the end.",
  "Finish him!",
  "Failure doesn't mean game over. Try again with more experience.",
  "Just one more game.",
  "Don't wish it were easier. Wish you were better.",
  "Don't play the game. Win it.",
  "One day my mom will understand that online games cannot be paused.",
  "That moment you finish a game and don't know what to do with your life."
};
static const int kQuoteCount = sizeof(kQuotes)/sizeof(kQuotes[0]);

static int   qIndex = 0;
static int   qScroll = 0;
static uint32_t qLastStep = 0;
static const uint32_t Q_STEP_MS = 50;
static const int Q_PAD = 24;

static uint32_t qLastChange = 0;
static const uint32_t Q_ROTATE_MS = 60000UL;

static inline void pickRandomQuote() {
  uint32_t r = (uint32_t)esp_random();
  qIndex = (int)(r % (uint32_t)kQuoteCount);
  qScroll = 0;
  qLastStep = 0;
  qLastChange = millis();
  if (g_dbg) Serial.printf("[DISPLAY] Quote -> #%d\n", qIndex);
}

// ===== Labels & helpers =====
static const char* trayLabel(int v) {
  switch (v & 0xFF) { case 0x00: return "Closed"; case 0x01: return "Open"; case 0x02: return "Busy"; default: return "Unknown"; }
}
static const char* encLabel(int v) {
  switch (v & 0xFF) { case 0x45: return "Conexant"; case 0x6A: return "Focus"; case 0x70: return "Xcalibur"; default: return "Unknown"; }
}
static const char* xboxVerFromCode(int v) {
  switch (v & 0xFF) {
    case 0: return "v1.0"; case 1: return "v1.1"; case 2: return "v1.2";
    case 3: return "v1.3"; case 4: return "v1.4"; case 5: return "v1.5";
    case 6: return "v1.6"; default: return "Not reported";
  }
}
static bool av_is_hd(int v) { v &= 0xFF; return (v == 0x01) || (v == 0x02) || ((v & 0x0E) == 0x0A); }
static const char* avLabel(int v) {
  v &= 0xFF;
  switch (v) {
    case 0x00: return "SCART"; case 0x01: return "HDTV (Component)"; case 0x02: return "VGA"; case 0x03: return "RFU";
    case 0x04: return "Advanced (S-Video)"; case 0x06: return "Standard (Composite)"; case 0x07: return "Missing/Disconnected";
  }
  switch (v & 0x0E) {
    case 0x00: return "None/Disconnected"; case 0x02: return "Standard (Composite)";
    case 0x06: return "Advanced (S-Video)"; case 0x0A: return "HDTV (Component)"; case 0x0E: return "SCART";
  }
  return "Unknown";
}
static String sdSystemFromH(int h) { return (h >= 570) ? "PAL" : "NTSC"; }
static String modeFromRes(int w, int h, int avraw) {
  if (w >= 1900 && h == 1080) return "1080i";
  if (w == 1280 && h == 720)  return "720p";
  if ((w == 640 || w == 704 || w == 720) && h == 480) return av_is_hd(avraw) ? "480p" : "480i";
  if (w == 720 && h == 576)   return av_is_hd(avraw) ? "576p" : "576i";
  return "";
}
static String fmtResLine() {
  if (!EXT.have || EXT.width <= 0 || EXT.height <= 0) return "—";
  String mode = modeFromRes(EXT.width, EXT.height, EXT.av);
  if (mode.length()) {
    if (mode.startsWith("480") || mode.startsWith("576")) mode += " " + sdSystemFromH(EXT.height);
    return String(EXT.width) + "x" + String(EXT.height) + " (" + mode + ")";
  }
  return String(EXT.width) + "x" + String(EXT.height);
}

// ===== ConsoleMods serial parsing and version guess =====
static bool parseSerialYWWFF(const String& s, int& outYear, int& outWeek, int& outFactory) {
  int len = s.length();
  int run = 0, end = -1;
  for (int i = len - 1; i >= 0; --i) {
    if (isdigit((unsigned char)s[i])) { run++; if (run == 5) { end = i + 5; break; } }
    else run = 0;
  }
  if (end < 0) return false;
  int start = end - 5;
  int Y   = s[start] - '0';
  int WW  = (s[start+1]-'0')*10 + (s[start+2]-'0');
  int FF  = (s[start+3]-'0')*10 + (s[start+4]-'0');
  if (WW < 1 || WW > 53) return false;
  outYear = 2000 + Y; outWeek = WW; outFactory = FF;
  return true;
}
static String versionFromYearWeek(int year, int week) {
  if (year == 2001) return "v1.0";
  if (year == 2002) { if (week <= 43) return "v1.0"; if (week <= 47) return "v1.1"; return "v1.2"; }
  if (year == 2003) { if (week <= 8)  return "v1.2"; if (week <= 30) return "v1.3"; return "v1.4"; }
  if (year == 2004) { if (week <= 10) return "v1.4"; if (week <= 37) return "v1.6"; return "v1.6b"; }
  if (year >= 2005) return "v1.6b";
  return "Not reported";
}
static String guessFromSerialAndEncoder(int encRaw, const String& serial) {
  int year=0, week=0, factory=0;
  bool ok = parseSerialYWWFF(serial, year, week, factory);
  const int enc = (encRaw & 0xFF);
  auto encSuggest = [&]() -> String {
    if (enc == 0x70) return "v1.6";
    if (enc == 0x6A) return "v1.4";
    if (enc == 0x45) return "v1.0–1.3";
    return "";
  };
  if (!ok) {
    String e = encSuggest();
    return e.length() ? e : "Not reported";
  }
  if (factory == 3)  return "v1.0"; // Hungary
  if (factory == 2)  return (year < 2002 || (year == 2002 && week < 44)) ? "v1.0" : "v1.1"; // Mexico
  String ywGuess = versionFromYearWeek(year, week);
  if (enc == 0x70) { if (year >= 2004 && week >= 38) return "v1.6b"; return "v1.6"; }
  if (enc == 0x6A) { if (ywGuess.startsWith("v1.0") || ywGuess.startsWith("v1.1") || ywGuess.startsWith("v1.2") || ywGuess.startsWith("v1.3")) return "v1.4"; }
  if (enc == 0x45) { if (ywGuess.startsWith("v1.4") || ywGuess.startsWith("v1.6")) return "v1.3"; }
  return ywGuess;
}
static String fmtXboxVersion() {
  if (EXT.xboxver >= 0 && EXT.xboxver <= 6) return String(xboxVerFromCode(EXT.xboxver));
  if (!EE.have || EE.serial.length() == 0) {
    const int e = (EXT.enc & 0xFF);
    if (e == 0x70) return "v1.6";
    if (e == 0x6A) return "v1.4";
    if (e == 0x45) return "v1.0–1.3";
    return "Not reported";
  }
  return guessFromSerialAndEncoder(EXT.enc, EE.serial);
}

// ===== Boot glyph (full-screen) =====
static void drawBootGlyph() {
  g->clearBuffer();
  #if defined(DC_LOGO_WIDTH) && defined(DC_LOGO_HEIGHT) && defined(DC_LOGO_BITS)
    g->drawXBM(0, 0, DC_LOGO_WIDTH, DC_LOGO_HEIGHT, DC_LOGO_BITS);
  #elif defined(dc_logo_width) && defined(dc_logo_height) && defined(dc_logo_bits)
    g->drawXBM(0, 0, dc_logo_width, dc_logo_height, dc_logo_bits);
  #elif defined(dc_logoDC_logo)
    g->drawXBM(0, 0, 128, 64, dc_logoDC_logo);
  #endif
  g->sendBuffer();
}

// ===== UDP ingest =====
static void onPacket() {
  // Drain all pending packets from the UDP ring so we never miss updates.
  TypeDUDP::Packet pk;
  while (TypeDUDP::next(pk)) {
    const uint16_t dst = pk.dst_port;
    const size_t   n   = pk.rx_len;

    everAnyPacket = true;
    lastAnyAt     = pk.ts_ms;   // use precise timestamp carried with the packet

    if ((dst == UDP_TYPED_DEFAULT_PORT_A) || (n == 44)) {
      if (n >= 44) {
        struct __attribute__((packed)) { int32_t fan, cpu, amb; char app[32]; } m{};
        memcpy(&m, pk.data, sizeof(m));
        MAIN.have = true;
        haveMain  = true;
        MAIN.fan   = constrain(m.fan, 0, 100);
        MAIN.cpu_c = m.cpu;
        MAIN.amb_c = m.amb;
        memcpy(MAIN.app, m.app, 32); MAIN.app[32] = 0;
        if (g_dbg) Serial.printf("[DISPLAY] MAIN fan=%d cpu=%d amb=%d app='%s'\n",
                                 MAIN.fan, MAIN.cpu_c, MAIN.amb_c, MAIN.app);
      }
      continue;
    }

    if ((dst == UDP_TYPED_DEFAULT_PORT_B) || (n == 28)) {
      if (n >= 28) {
        struct __attribute__((packed)) { int32_t t,a,pic,xb,enc,x6,x7; } e{};
        memcpy(&e, pk.data, sizeof(e));
        EXT.have = true;
        EXT.tray = e.t;
        EXT.av   = e.a;
        EXT.xboxver = e.xb;
        auto looks_enc = [](int v)->bool { v&=0xFF; return v==0x45 || v==0x6A || v==0x70; };
        int cands[3] = { e.enc, e.x6, e.x7 };
        int enc=-1,w=0,h=0;
        if (looks_enc(cands[0]))      { enc=cands[0]; w=cands[1]; h=cands[2]; }
        else if (looks_enc(cands[1])) { enc=cands[1]; w=cands[0]; h=cands[2]; }
        else if (looks_enc(cands[2])) { enc=cands[2]; w=cands[0]; h=cands[1]; }
        else                          { enc = e.enc;  w=e.x6;     h=e.x7;     }
        EXT.enc = enc; EXT.width = w; EXT.height = h;
        if (g_dbg) {
          Serial.printf("[DISPLAY] EXT tray=%d av=0x%02X xb=%d enc=0x%02X w=%d h=%d\n",
                        EXT.tray, EXT.av & 0xFF, EXT.xboxver, EXT.enc & 0xFF, EXT.width, EXT.height);
        }
      }
      continue;
    }

    if ((dst == UDP_TYPED_DEFAULT_PORT_C) || (n >= 3 && pk.data[0]=='E' && pk.data[1]=='E' && pk.data[2]==':')) {
      String s = String(pk.data); s.trim();
      if (s.startsWith("EE:")) {
        EE.have = true;
        int pos = 3;
        while (pos < (int)s.length()) {
          int eq = s.indexOf('=', pos); if (eq < 0) break;
          String key = s.substring(pos, eq);
          int bar = s.indexOf('|', eq+1);
          String val = (bar<0) ? s.substring(eq+1) : s.substring(eq+1, bar);
          key.trim(); val.trim();
          if      (key.equalsIgnoreCase("SN")  || key.equalsIgnoreCase("SER")) EE.serial = val;
          else if (key.equalsIgnoreCase("MAC"))   EE.mac    = val;
          else if (key.equalsIgnoreCase("REG"))   EE.region = val;
          pos = (bar<0) ? s.length() : (bar + 1);
        }
        if (g_dbg) {
          Serial.printf("[DISPLAY] EE SN=%s MAC=%s REG=%s\n",
                        EE.serial.c_str(), EE.mac.c_str(), EE.region.c_str());
        }
      }
      continue;
    }
  }
 }

// ===== Text helpers =====
static String ellipsize(const String& s, int maxW) {
  g->setFont(u8g2_font_6x12_tf);
  int w = g->getStrWidth(s.c_str());
  if (w <= maxW) return s;
  String out = s;
  const String dots = "...";
  int dotsW = g->getStrWidth(dots.c_str());
  while (out.length() && g->getStrWidth(out.c_str()) + dotsW > maxW) out.remove(out.length()-1);
  return out + dots;
}
static void kvRow_6x12(int x, int y, const char* key, const String& val, int totalW) {
  g->setFont(u8g2_font_6x12_tf);
  g->setCursor(x, y); g->print(key);
  int kw = g->getStrWidth(key);
  int avail = totalW - kw; if (avail < 0) avail = 0;
  String vfit = ellipsize(val, avail);
  g->setCursor(x + kw, y); g->print(vfit);
}
static void kvRow_5x8(int x, int y, const char* key, const String& val, int totalW) {
  g->setFont(u8g2_font_5x8_tf);
  g->setCursor(x, y); g->print(key);
  int kw = g->getStrWidth(key);
  int avail = totalW - kw; if (avail < 0) avail = 0;
  String vfit = val;
  const String dots = "...";
  if (g->getStrWidth(vfit.c_str()) > avail) {
    while (vfit.length() && g->getStrWidth((vfit + dots).c_str()) > avail) vfit.remove(vfit.length()-1);
    vfit += dots;
  }
  g->setCursor(x + kw, y); g->print(vfit);
}

// Wi-Fi quality label from RSSI
static const char* rssiQuality(int rssiDbm) {
  if (rssiDbm >= -55) return "Excellent";
  if (rssiDbm >= -67) return "Good";
  if (rssiDbm >= -75) return "Fair";
  return "Poor";
}

// ===== Battery widget helper (inline on Fan row) =====
static void drawBatteryInlineRight(int textEndX, int rowBaselineY, int rowRightX) {
  if (!lc_ok || isnan(lc_pct)) return;
  const int w = 20, h = 10, tipW = 2, pad = 2;
  int iconX = rowRightX - (w + tipW);
  int iconTop = rowBaselineY - h; if (iconTop < 0) iconTop = 0;
  g->drawFrame(iconX, iconTop, w, h);
  g->drawBox(iconX + w, iconTop + (h/2 - 2), tipW, 4);
  int innerW = w - 2;
  int fill = (int)((innerW * constrain((int)round(lc_pct), 0, 100)) / 100);
  if (fill > 0) g->drawBox(iconX + 1, iconTop + 1, fill, h - 2);

  g->setFont(u8g2_font_6x12_tf);
  String pct = String((int)round(lc_pct)) + "%";
  int tw = g->getStrWidth(pct.c_str());
  int spaceAvail = iconX - pad - textEndX;
  if (tw <= spaceAvail) { int tx = iconX - pad - tw; g->setCursor(tx, rowBaselineY); g->print(pct); }
}

// ===== Screensaver (bouncing “Sleeping...”) =====
static bool saverActive = false;
static const uint32_t SAVER_STEP_MS = 45;
static uint32_t saverLastStep = 0;

static const char* saverMsg = "Sleeping...";
static int saverX = 0, saverY = 0;
static int saverDX = 2, saverDY = 1;
static int saverW = 0, saverH = 0, saverAsc = 0;

static void startScreensaver() {
  saverActive = true;
  g->setFont(u8g2_font_7x13B_tf);
  saverAsc = g->getAscent();
  saverH = saverAsc - g->getDescent();
  saverW = g->getStrWidth(saverMsg);
  uint32_t r = (uint32_t)esp_random();
  int maxX = 128 - saverW;
  int maxY = 64 - saverH;
  if (maxX < 0) maxX = 0;
  if (maxY < 0) maxY = 0;
  saverX = (int)(r % (uint32_t)(maxX + 1));
  saverY = saverAsc + (int)((r >> 8) % (uint32_t)(maxY + 1));
  saverDX = ((r & 1) ? 2 : -2);
  saverDY = ((r & 2) ? 1 : -1);
  saverLastStep = 0;
}
static void stopScreensaver() { saverActive = false; }

static void drawScreensaverFrame() {
  uint32_t now = millis();
  if (now - saverLastStep >= SAVER_STEP_MS) {
    saverLastStep = now;
    saverX += saverDX;
    saverY += saverDY;
    if (saverX < 0) { saverX = 0; saverDX = -saverDX; }
    if (saverX + saverW > 128) { saverX = 128 - saverW; saverDX = -saverDX; }
    int top = saverY - saverAsc;
    int bot = top + saverH;
    if (top < 0) { top = 0; saverY = top + saverAsc; saverDY = -saverDY; }
    if (bot > 64) { bot = 64; saverY = bot - saverH + saverAsc; saverDY = -saverDY; }
  }
  g->clearBuffer();
  g->setFont(u8g2_font_7x13B_tf);
  int top = saverY - saverAsc;
  g->drawFrame(saverX - 2, top - 2, saverW + 4, saverH + 4);
  g->setCursor(saverX, saverY);
  g->print(saverMsg);
  g->sendBuffer();
}

// ===== Screen layouts =====
static void drawQuoteTicker(int x, int y, int w) {
  g->setFont(u8g2_font_5x8_tf);
  int avail = w; if (avail <= 0) return;
  const char* text = kQuotes[qIndex];
  int tw = g->getStrWidth(text);
  if (tw <= avail) { g->setCursor(x, y); g->print(text); return; }
  uint32_t now = millis();
  if (now - qLastStep >= Q_STEP_MS) {
    qLastStep = now; qScroll = (qScroll + 2); int cycle = tw + Q_PAD; if (cycle > 0) qScroll %= cycle;
  }
  int startX = x - qScroll;
  g->setCursor(startX, y); g->print(text);
  int secondX = startX + tw + Q_PAD;
  if (secondX < x + avail) { g->setCursor(secondX, y); g->print(text); }
}

static void drawMainScreen(int xOffset=0) {
  g->clearBuffer();
  const int SCRW=128, L=2, RW=SCRW-2*L;
  const int rowRight = L + xOffset + RW;

  int y = 12;
  kvRow_6x12(L + xOffset, y, "App: ", String(MAIN.app), RW); y += 12;

  // Fan row + battery to the right
  {
    g->setFont(u8g2_font_6x12_tf);
    const char* key = "Fan: ";
    String val = String(MAIN.fan) + "%";
    int xKey = L + xOffset;
    g->setCursor(xKey, y); g->print(key);
    int xVal = xKey + g->getStrWidth(key);
    g->setCursor(xVal, y); g->print(val);
    int textEnd = xVal + g->getStrWidth(val.c_str());
    drawBatteryInlineRight(textEnd, y, rowRight);
  }
  y += 12;

  kvRow_6x12(L + xOffset,            y, "CPU: ", String(MAIN.cpu_c) + " C", RW/2 - 2);
  kvRow_6x12(L + xOffset + RW/2 + 2, y, "Amb: ", String(MAIN.amb_c) + " C", RW/2 - 2);
  y += 12;

  kvRow_6x12(L + xOffset, y, "Res: ", fmtResLine(), RW);

  drawQuoteTicker(L + xOffset, 60, RW);
  g->sendBuffer();
}

static void drawSecondScreen(int xOffset=0) {
  g->clearBuffer();
  const int SCRW=128, L=2, RW=SCRW-2*L;
  int y = 10;

  kvRow_5x8(L + xOffset, y, "Tray: ",    EXT.have ? String(trayLabel(EXT.tray))  : String("—"), RW); y += 8;
  kvRow_5x8(L + xOffset, y, "AV: ",      EXT.have ? String(avLabel(EXT.av))      : String("—"), RW); y += 8;
  kvRow_5x8(L + xOffset, y, "Xbox: ",    fmtXboxVersion(),                                       RW); y += 8;
  kvRow_5x8(L + xOffset, y, "Encoder: ", EXT.have ? String(encLabel(EXT.enc))    : String("—"), RW); y += 8;
  kvRow_5x8(L + xOffset, y, "Serial: ",  EE.have ? EE.serial : String("—"), RW); y += 8;
  kvRow_5x8(L + xOffset, y, "MAC: ",     EE.have ? EE.mac    : String("—"), RW); y += 8;
  kvRow_5x8(L + xOffset, y, "Region: ",  EE.have ? EE.region : String("—"), RW);

  g->sendBuffer();
}

static void drawHealthScreen(int xOffset=0) {
  g->clearBuffer();
  const int SCRW=128, L=2, RW=SCRW-2*L;

  g->setFont(u8g2_font_6x12_tf);
  g->setCursor(L + xOffset, 12); g->print("Status");

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
  String wifiLine = String(rssi) + " dBm (" + rssiQuality(rssi) + ")";
  kvRow_6x12(L + xOffset, 28, "WiFi: ", wifiLine, RW);

  size_t heapFree  = ESP.getFreeHeap();
  String memLine = String(heapFree / 1024) + " KB";
  kvRow_6x12(L + xOffset, 40, "Free: ", memLine, RW);

  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP(); char buf[24];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    kvRow_6x12(L + xOffset, 52, "IP: ", String(buf), RW);
  } else {
    kvRow_6x12(L + xOffset, 52, "IP: ", String("(disconnected)"), RW);
  }

  g->sendBuffer();
}

// Text-only, icon-free weather screen for 128x64
static void drawWeatherScreen(int xOffset /*=0*/) {
  g->clearBuffer();
  const int Wd=128, L=2, RW=Wd - 2*L;

  // ---------- Header: place or coords ----------
  g->setFont(u8g2_font_6x12_tf);
  String head = (strlen(W.place)
                  ? String(W.place)
                  : (isnan(W.lat)||isnan(W.lon) ? String("Weather")
                                                : (String(W.lat,2) + "," + String(W.lon,2))));
  // ellipsize to available width
  String headFit = ellipsize(head, RW);
  int headW = g->getStrWidth(headFit.c_str());
  int headX = L + xOffset + (RW - headW)/2;
  int headY = 11;                        // baseline around row 1
  g->setCursor(headX, headY);
  g->print(headFit);

  // ---------- Big temperature (centered) ----------
  g->setFont(u8g2_font_logisoso16_tf);   // tall but still leaves room for 3 lines total
  char tbuf[16];
  if (!isnan(W.temp)) {
    // e.g. "72°F" or "22°C"
    snprintf(tbuf, sizeof(tbuf), "%.0f%c%c", W.temp, (char)0xB0, (W.units=='F'?'F':'C'));
  } else {
    snprintf(tbuf, sizeof(tbuf), "--%c%c", (char)0xB0, (W.units=='F'?'F':'C'));
  }
  int tempW = g->getStrWidth(tbuf);
  int tempX = L + xOffset + (RW - tempW)/2;
  int tempY = 34;                        // visually centered under header
  g->setCursor(tempX, tempY);
  g->print(tbuf);

  // ---------- Condition text (centered) ----------
  g->setFont(u8g2_font_6x12_tf);
  String cond = String(labelForCode(W.code));  // falls back to "—"
  String condFit = ellipsize(cond, RW);
  int condW = g->getStrWidth(condFit.c_str());
  int condX = L + xOffset + (RW - condW)/2;
  int condY = 48;
  g->setCursor(condX, condY);
  g->print(condFit);

  // ---------- Bottom metrics row (centered) ----------
  // Compact: "H45%  W6mph" or "H--  W--"
  g->setFont(u8g2_font_5x8_tf);
  String hum  = (W.rh >= 0) ? (String("H") + W.rh + "%") : "H--";
  String wind = String("W") + (isnan(W.wind) ? String("--") : String(W.wind,0)) + (W.units=='F' ? "mph" : "kmh");

  // Build a single line and trim if needed
  String tail = hum + "  " + wind;
  // If still too wide, drop spaces, then (rarely) truncate wind units
  if (g->getStrWidth(tail.c_str()) > RW) {
    tail = hum + " " + wind;
    if (g->getStrWidth(tail.c_str()) > RW) {
      // last resort: shorten units to a single letter
      wind = String("W") + (isnan(W.wind) ? String("--") : String(W.wind,0)) + (W.units=='F' ? "m" : "k");
      tail = hum + " " + wind;
    }
  }
  int tailW = g->getStrWidth(tail.c_str());
  int tailX = L + xOffset + (RW - tailW)/2;
  int tailY = 61;                        // near bottom baseline
  g->setCursor(tailX, tailY);
  g->print(tail);

  g->sendBuffer();
}


// ===== Transitions (slide-only) =====
static void drawWithOffsets(Screen s, int x) {
  if      (s==Screen::MAIN)    drawMainScreen(x);
  else if (s==Screen::SECOND)  drawSecondScreen(x);
  else if (s==Screen::HEALTH)  drawHealthScreen(x);
  else                         drawWeatherScreen(x);
}
static void doTransition(Screen to) {
  uint32_t r = (uint32_t)esp_random();
  nextXition = (r & 1) ? Xition::SLIDE_IN_RIGHT : Xition::SLIDE_IN_LEFT;
  const int SCRW=128, step=16;
  if (nextXition == Xition::SLIDE_IN_RIGHT) {
    for (int x=SCRW; x>=0; x-=step) { drawWithOffsets(to, x); delay(12); }
  } else {
    for (int x=-SCRW; x<=0; x+=step){ drawWithOffsets(to, x); delay(12); }
  }
}

// ===== API =====
void setHoldTimes(uint32_t main_ms, uint32_t second_ms) {
  HOLD_MAIN_MS   = main_ms   ? main_ms   : HOLD_MAIN_MS;
  HOLD_SECOND_MS = second_ms ? second_ms : HOLD_SECOND_MS;
}
void setDebug(bool on) { g_dbg = on; }

void begin(U8G2* u8) {
  g = u8;
  esp_fill_random(&lastDraw, sizeof(lastDraw));
  randomSeed((uint32_t)lastDraw ^ millis());
  cur = Screen::WAITING;
  nextSwitchAt = 0;
  everAnyPacket = false;
  haveMain = false;
  lastAnyAt = 0;

  // Load weather prefs once
  loadWeatherPrefs();

  pickRandomQuote();
  if (g_dbg) Serial.printf("[DISPLAY] begin (waiting for UDP data)\n");
}

static Screen nextScreen(Screen s) {
  // Include WEATHER only if enabled; otherwise skip it.
  switch (s) {
    case Screen::MAIN:   return Screen::SECOND;
    case Screen::SECOND: return Screen::HEALTH;
    case Screen::HEALTH: return W.enabled ? Screen::WEATHER : Screen::MAIN;
    case Screen::WEATHER:return Screen::MAIN;
    default:             return Screen::MAIN;
  }
}
static uint32_t holdFor(Screen s) {
  switch (s) {
    case Screen::MAIN:    return HOLD_MAIN_MS;
    case Screen::SECOND:  return HOLD_SECOND_MS;
    case Screen::HEALTH:  return HOLD_HEALTH_MS;
    case Screen::WEATHER: return HOLD_WEATHER_MS;
    default:              return HOLD_MAIN_MS;
  }
}

void loop() {
  if (!g) return;

  // Fuel gauge maintenance (init + periodic read)
  maybeInitGauge();
  updateGaugeReading();

  // Weather refresh cadence (non-blocking most of the time)
  if (W.enabled) {
    uint32_t noww = millis();
    if (W.last_fetch == 0 || noww - W.last_fetch >= W.refresh_ms) {
      fetchWeatherNow();
    }
  }

  // Ingest any new UDP packets (drain ring buffer)
  if (TypeDUDP::available()) onPacket();

  // Compute inactivity from ANY port (use our timestamp, not an "age" misinterpreted as a time)
  const uint32_t now = millis();
  const bool noAny5m = (everAnyPacket && lastAnyAt && (now - lastAnyAt >= NO_PACKET_SAVER_MS));
  const bool noAny2m = (everAnyPacket && lastAnyAt && (now - lastAnyAt >= NO_PACKET_LOGO_MS));

  // If we've never seen ANY packet, or MAIN hasn't arrived yet, show logo
  if (!everAnyPacket || !haveMain) {
    drawBootGlyph();
    return;
  }

  // Inactivity gating
  if (noAny5m) {
    if (!saverActive) startScreensaver();
    drawScreensaverFrame();
    return;
  }
  if (saverActive) stopScreensaver();
  if (noAny2m) { drawBootGlyph(); return; }

  // Normal screen scheduler
  if (cur == Screen::WAITING) {
    cur = Screen::MAIN;
    nextSwitchAt = now + holdFor(cur);
    pickRandomQuote();
    if (g_dbg) Serial.println("[DISPLAY] first MAIN -> MAIN");
    doTransition(cur);
    return;
  }

  if (now >= nextSwitchAt) {
    cur = nextScreen(cur);
    nextSwitchAt = now + holdFor(cur);
    if (cur == Screen::MAIN) pickRandomQuote();
    if (g_dbg) {
      const char* nm = (cur==Screen::MAIN)?"MAIN":(cur==Screen::SECOND)?"SECOND":(cur==Screen::HEALTH)?"HEALTH":"WEATHER";
      Serial.printf("[DISPLAY] switch -> %s\n", nm);
    }
    doTransition(cur);
    return;
  }

  if (cur == Screen::MAIN && (now - qLastChange >= Q_ROTATE_MS)) pickRandomQuote();

  if (now - lastDraw >= DRAW_INTERVAL_MS) {
    lastDraw = now;
    if      (cur == Screen::MAIN)    drawMainScreen(0);
    else if (cur == Screen::SECOND)  drawSecondScreen(0);
    else if (cur == Screen::HEALTH)  drawHealthScreen(0);
    else                             drawWeatherScreen(0);
  }
}

} // namespace TypeDDisplay
