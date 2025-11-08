#include "insignia.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  #include <WiFi.h>
  #include <FS.h>
  #include <SPIFFS.h>
#endif
#include <vector>
#include <algorithm>
#include <time.h>

namespace Insignia {

// =================== Config / constants ===================
static bool g_dbg = false;

static String BASE = "http://darkone83.myddns.me:8080/xbox"; // user-provided base (no trailing slash)
static String WORK_ROOT;                                     // probed root that has /data/search.json

// Screen geometry & timing
static const int SCR_W=128, SCR_H=64;
static const int TOP_LINE_Y=12, RULE_Y=14, CONTENT_TOP=RULE_Y+2;
static const int LINE_H = 9;         // u8g2_font_5x8_tf baseline step
static const int ASCENT_5x8 = 7;
static const uint32_t STEP_MS = 40;  // scroll cadence
static const int PIXELS_PER_STEP = 1;
static const uint32_t BOARD_MIN_DWELL_MS = 3000; // min dwell per board
static const uint32_t FREEZE_MS = 750;           // one freeze per board on entry
static const uint32_t MODEL_DWELL_MS = 12000;    // min dwell per *variant* before switching model

// ---- header aliases & options (match emulator) ----
static const char* RANK_KEYS[] = {"rank", "#", "pos", "position", "place", nullptr};
static const char* NAME_KEYS[] = {"name","player","gamertag","gamer","tag","alias","username","user","gt","account", nullptr};
// Prefer these keys for the value-only metric (case-insensitive)
static const char* PREFER_METRIC[] = {"score","points","rating","time","best time","laps","wins","value", nullptr};
// 0 = unlimited rows per board (like emulator)
static const int MAX_ROWS_PER_BOARD = 0;

// Cache policy (tunable via setters)
#if defined(ARDUINO) || defined(ESP_PLATFORM)
static const char* CACHE_DIR = "/insig";
static bool  fsReady = false;
static bool  flushOnBoot = false;
static size_t cacheMaxFiles = 32;                 // keep it small
static size_t cacheMaxBytes = 128 * 1024;         // 128 KB ceiling
static uint32_t cacheMaxAgeMs = 6UL*60*60*1000UL; // default 6h max age
// Per-resource TTLs
static const uint32_t TTL_SEARCH_MS = 6UL*60*60*1000UL; // 6h
static const uint32_t TTL_BYID_MS   = 2UL*60*1000UL;    // 2m
#endif

// =================== Runtime state ===================
static String curApp;       // current app name (from console)
static String gameTitle;    // pretty name
static bool   haveSearch = false;
static bool   resolved   = false;   // have a title pool
static bool   loaded     = false;   // model loaded

// When multiple IDs match (e.g., NTSC/PAL), rotate through them
static std::vector<String> titlePool; // 1..N title_ids
static int curTitleIdx = -1;          // index into titlePool
static uint32_t lastModelSwitch = 0;  // last time we swapped between NTSC/PAL variants

struct Row {
  String rank, name;
  String metric;                 // chosen value-only metric
  std::vector<String> extras;    // remaining "key=value"
};
struct Board { String name; std::vector<Row> rows; };
static std::vector<Board> boards;

static int   curBoard = -1;
static float scrollY  = 0.f;
static uint32_t lastStep = 0;
static uint32_t lastBoardSwitch = 0;
static uint32_t freezeUntilMs = 0;
static uint32_t lastFetchMs = 0;

// =================== Helpers ===================
static inline bool netReady() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  return WiFi.status() == WL_CONNECTED;
#else
  return true;
#endif
}
static String lc(const String& s){ String t=s; t.toLowerCase(); return t; }

static bool in_list_ci(const String& s, const char* const* keys){
  String L = s; L.toLowerCase();
  for (int i=0; keys[i]; ++i) if (L == keys[i]) return true;
  return false;
}
static int metric_pref(const String& key){
  String L = key; L.toLowerCase();
  for (int i=0; PREFER_METRIC[i]; ++i) if (L == PREFER_METRIC[i]) return i;
  return 100; // deprioritize unknowns
}

static int rankKey(const String& rs){
  int val=1000000000, n=0; bool any=false;
  for (size_t i=0;i<rs.length();++i){
    char c=rs[i];
    if (c>='0'&&c<='9'){ n=n*10+(c-'0'); any=true; }
    else if (any) break;
  }
  if (any) val=n;
  return val;
}

static void resetRuntime(bool keepRoot=false) {
  (void)keepRoot;
  boards.clear();
  curBoard = -1;
  gameTitle = "";
  haveSearch = false;
  resolved   = false;
  loaded     = false;
  titlePool.clear();
  curTitleIdx = -1;
  scrollY = 0.f;
  lastStep = 0;
  lastBoardSwitch = 0;
  freezeUntilMs = 0;
  lastModelSwitch = 0;
  // keep WORK_ROOT unless BASE changes
}

// =================== SPIFFS cache (ESP only) ===================
#if defined(ARDUINO) || defined(ESP_PLATFORM)
static void ensureFS() {
  if (!fsReady) {
    fsReady = SPIFFS.begin(true);
    if (g_dbg) Serial.printf("[INSIGNIA] SPIFFS %s\n", fsReady ? "mounted" : "MOUNT FAIL");
    if (fsReady && flushOnBoot) {
      // one-time flush on boot
      File root = SPIFFS.open("/");
      for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        String p = f.path();
        if (p.startsWith(CACHE_DIR)) SPIFFS.remove(p);
      }
      if (g_dbg) Serial.println("[INSIGNIA] cache flushed on boot");
      flushOnBoot = false; // only once
    }
  }
}

static String sanitizeKey(const String& url) {
  String k=url;
  k.replace("://","__");
  const char *bad="/?:&=%#";
  for (const char* p=bad; *p; ++p) { k.replace(String(*p), "_"); }
  if (!k.startsWith("/")) k = "/" + k;
  return String(CACHE_DIR) + k;
}

static size_t dirSize(const char* dir, size_t& files) {
  size_t bytes=0; files=0;
  File root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (String(f.path()).startsWith(dir)) { bytes += f.size(); files++; }
  }
  return bytes;
}

static void pruneCache() {
  ensureFS(); if (!fsReady) return;
  // Remove too-old files, then enforce count/size limits
  std::vector<std::pair<String, time_t>> entries;
  File root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = f.path();
    if (!p.startsWith(CACHE_DIR)) continue;
    entries.emplace_back(p, f.getLastWrite());
  }
  // Age-based prune
  time_t nowt = time(nullptr);
  for (auto& e : entries) {
    File f = SPIFFS.open(e.first);
    if (!f) continue;
    bool tooOld = (nowt>0 && e.second>0) && ((uint32_t)((nowt - e.second)*1000UL) > cacheMaxAgeMs);
    f.close();
    if (tooOld) SPIFFS.remove(e.first);
  }
  // Re-scan for size/count
  size_t files=0, bytes = dirSize(CACHE_DIR, files);
  if (files <= cacheMaxFiles && bytes <= cacheMaxBytes) return;

  // Trim oldest first
  entries.clear();
  root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = f.path();
    if (!p.startsWith(CACHE_DIR)) continue;
    entries.emplace_back(p, f.getLastWrite());
  }
  std::sort(entries.begin(), entries.end(),
            [](auto& a, auto& b){ return a.second < b.second; }); // oldest first

  for (auto& e : entries) {
    if (files <= cacheMaxFiles && bytes <= cacheMaxBytes) break;
    File f = SPIFFS.open(e.first, FILE_READ);
    size_t sz = f ? f.size() : 0;
    if (f) f.close();
    SPIFFS.remove(e.first);
    if (files) files--;
    if (bytes >= sz) bytes -= sz; else bytes = 0;
  }
}

static bool cacheWrite(const String& url, const String& body) {
  ensureFS(); if (!fsReady) return false;
  File f = SPIFFS.open(sanitizeKey(url), FILE_WRITE);
  if (!f) return false;
  size_t n = f.print(body);
  f.close();
  pruneCache();
  return n == (size_t)body.length();
}

static bool cacheRead(const String& url, String& out, uint32_t maxAgeMs, bool allowStaleIfNoNet) {
  ensureFS(); if (!fsReady) return false;
  String path = sanitizeKey(url);
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;
  time_t mtime = f.getLastWrite();
  out = f.readString(); f.close();
  time_t nowt = time(nullptr);
  bool fresh = (mtime>0 && nowt>0 && (uint32_t)((nowt - mtime) * 1000UL) <= maxAgeMs);
  if (fresh) return true;
  return allowStaleIfNoNet;
}
#endif // SPIFFS

// =================== HTTP ===================
static bool httpGet(const String& url, String& out) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}

// =================== Root probing (match emulator) ===================
static void candidateRoots(std::vector<String>& out) {
  out.clear();
  String b = BASE; b.trim();
  if (b.length()==0) return;
  auto add=[&](const String& s){
    String t=s;
    while (t.endsWith("/")) t.remove(t.length()-1);
    if (t.length() && std::find(out.begin(),out.end(),t)==out.end()) out.push_back(t);
  };
  add(b);
  if (b.endsWith("/data")) { String p=b; p.remove(p.length()-5); add(p); }
  add(b + "/xbox");
  add(b + "/xbox/data");
}

static bool probeWorkRoot() {
  if (!netReady()) return false;
  std::vector<String> roots;
  candidateRoots(roots);
  for (auto& r : roots) {
    String url = r + "/data/search.json";
    String body;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    if (cacheRead(url, body, TTL_SEARCH_MS, true)) {
      DynamicJsonDocument tmp(2048);
      if (!deserializeJson(tmp, body)) { WORK_ROOT=r; return true; }
    }
#endif
    if (httpGet(url, body)) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
      cacheWrite(url, body);
#endif
      DynamicJsonDocument tmp(2048);
      if (!deserializeJson(tmp, body)) { WORK_ROOT=r; return true; }
    }
  }
  return false;
}

// =================== Resolve by App → build title pool ===================
static bool resolveTitlePool() {
  if (!netReady() && WORK_ROOT.length()==0) return false;
  if (WORK_ROOT.length()==0 && !probeWorkRoot()) return false;

  String url = WORK_ROOT + "/data/search.json";
  String body;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  if (!cacheRead(url, body, TTL_SEARCH_MS, false)) {
    if (httpGet(url, body)) cacheWrite(url, body);
    else if (!cacheRead(url, body, 0, true)) return false;
  }
#else
  if (!httpGet(url, body)) return false;
#endif

  DynamicJsonDocument doc(1 << 15);
  if (deserializeJson(doc, body)) return false;

  String want = lc(curApp);
  String wantSlug = want; wantSlug.replace(' ', '-');

  // First collect *strong* matches into a pool (exact name, exact name_lc, exact slug).
  std::vector<std::pair<String,int>> scored; // id, score
  std::vector<String> poolStrong;

  for (JsonObject it : doc.as<JsonArray>()) {
    String id   = it["title_id"] | "";
    String name = it["name"]     | "";
    String nlc  = it["name_lc"]  | "";
    String slug = it["slug"]     | "";

    int score = 0;
    if (lc(name) == want) score = 100;
    else if (nlc == want) score = 95;
    else if (slug == wantSlug) score = 90;
    else if (lc(name).indexOf(want) >= 0) score = 80;
    else if (want.indexOf(lc(name)) >= 0) score = 70;

    if (score >= 90) poolStrong.push_back(id);      // exact-equivalent bucket
    if (score >= 70) scored.emplace_back(id,score); // keep as fallback
  }

  titlePool.clear();
  if (!poolStrong.empty()) {
    std::sort(poolStrong.begin(), poolStrong.end());
    poolStrong.erase(std::unique(poolStrong.begin(), poolStrong.end()), poolStrong.end());
    titlePool = poolStrong;                          // may include NTSC & PAL variants
  } else if (!scored.empty()) {
    std::sort(scored.begin(), scored.end(),
              [](auto&a, auto&b){ return a.second>b.second; });
    titlePool.push_back(scored.front().first);
  }

  if (titlePool.empty()) return false;

#if defined(ESP_PLATFORM)
  curTitleIdx = (int)(esp_random() % titlePool.size());
#else
  curTitleIdx = (int)(rand() % titlePool.size());
#endif

  haveSearch = true;
  resolved   = true;
  if (g_dbg) {
    Serial.printf("[INSIGNIA] pool size=%d (root=%s)\n",
                  (int)titlePool.size(), WORK_ROOT.c_str());
  }
  return true;
}

// =================== Load one variant model (robust, matches emulator) ===================
static bool loadGameModel(const String& titleId) {
  String url = WORK_ROOT + "/data/by_id/" + titleId + ".json";
  String body;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  if (!cacheRead(url, body, TTL_BYID_MS, false)) {
    if (httpGet(url, body)) cacheWrite(url, body);
    else if (!cacheRead(url, body, 0, true)) return false;
  }
#else
  if (!httpGet(url, body)) return false;
#endif

  DynamicJsonDocument doc(1 << 16);
  if (deserializeJson(doc, body)) return false;

  boards.clear(); curBoard=-1; gameTitle="";
  gameTitle = String(doc["game_title"] | "");

  JsonArray sbs = doc["scoreboards"].as<JsonArray>();
  if (sbs.isNull()) return false;

  for (JsonObject sb : sbs) {
    Board B; B.name = String(sb["name"] | "default");

    // ----- columns (may be missing/partial) -----
    std::vector<String> cols;
    if (sb["columns"].is<JsonArray>()) {
      for (JsonVariant v : sb["columns"].as<JsonArray>()) {
        const char* cs = v.as<const char*>(); cols.emplace_back(cs ? cs : "");
      }
    }

    JsonArray rowsA = sb["rows"].as<JsonArray>();
    if (rowsA.isNull()) continue;

    // derive columns from first dict row if absent
    if (cols.empty() && rowsA.size()>0 && rowsA[0].is<JsonObject>()) {
      JsonObject first = rowsA[0].as<JsonObject>();
      for (JsonPair kv : first) cols.emplace_back(kv.key().c_str());
    }

    // canonical indices
    int nameIdx = -1, rankIdx = -1;
    for (size_t i=0;i<cols.size();++i) {
      if (rankIdx < 0 && in_list_ci(cols[i], RANK_KEYS)) rankIdx = (int)i;
      if (nameIdx < 0 && in_list_ci(cols[i], NAME_KEYS)) nameIdx = (int)i;
    }

    auto isDeclared = [&](const String& key)->bool {
      for (auto& c : cols) if (c == key) return true;
      return false;
    };

    int pushed = 0;
    for (JsonVariant rv : rowsA) {
      Row row; std::vector<String> extras;

      if (rv.is<JsonObject>()) {
        JsonObject r = rv.as<JsonObject>();

        auto val_by_col = [&](int i)->String {
          if (i < 0 || i >= (int)cols.size()) return String();
          JsonVariant v = r[cols[i]];
          return v.isNull() ? String() : String(v.as<String>());
        };

        // Rank/Name via columns…
        row.rank = (rankIdx>=0) ? val_by_col(rankIdx) : String();
        row.name = (nameIdx>=0) ? val_by_col(nameIdx) : String();

        // …fallback via row keys if columns omit them
        if (row.rank.isEmpty()) {
          for (JsonPair kv : r) { if (in_list_ci(kv.key().c_str(), RANK_KEYS)) { row.rank = String(kv.value().as<String>()); break; } }
        }
        if (row.name.isEmpty()) {
          for (JsonPair kv : r) { if (in_list_ci(kv.key().c_str(), NAME_KEYS)) { row.name = String(kv.value().as<String>()); break; } }
        }
        if (row.rank.isEmpty()) row.rank = String((int)B.rows.size() + 1); // synthesize

        // declared non-rank/non-name columns first
        for (size_t i=0;i<cols.size();++i) {
          if ((int)i==rankIdx || (int)i==nameIdx) continue;
          String v = val_by_col((int)i);
          if (v.length()) extras.emplace_back(cols[i] + "=" + v);
        }
        // then any extra keys not in columns
        for (JsonPair kv : r) {
          String k = String(kv.key().c_str());
          if (!k.length() || isDeclared(k)) continue;
          String v = kv.value().isNull() ? String() : String(kv.value().as<String>());
          if (v.length()) extras.emplace_back(k + "=" + v);
        }

      } else if (rv.is<JsonArray>()) {
        JsonArray arr = rv.as<JsonArray>();
        auto val_at = [&](int i)->String {
          if (i < 0 || i >= (int)arr.size()) return String();
          JsonVariant v = arr[i]; return v.isNull() ? String() : String(v.as<String>());
        };

        row.rank = (rankIdx>=0) ? val_at(rankIdx) : String((int)B.rows.size() + 1);
        row.name = (nameIdx>=0) ? val_at(nameIdx) : String();

        for (size_t i=0;i<cols.size();++i) {
          if ((int)i==rankIdx || (int)i==nameIdx) continue;
          String v = val_at((int)i);
          if (v.length()) extras.emplace_back(cols[i] + "=" + v);
        }

      } else {
        // unknown shape -> treat whole thing as a name
        row.rank = String((int)B.rows.size() + 1);
        row.name = String(rv.as<String>());
      }

      // strip Rank/Name-like from extras to avoid echoes (e.g., "Name=…", "Pos=…")
      {
        std::vector<String> cleaned; cleaned.reserve(extras.size());
        for (auto& kv : extras) {
          int eq = kv.indexOf('=');
          String k = (eq>0) ? kv.substring(0,eq) : String();
          if (!k.length()) continue;
          if (in_list_ci(k, RANK_KEYS) || in_list_ci(k, NAME_KEYS)) continue;
          cleaned.push_back(kv);
        }
        extras.swap(cleaned);
      }

      // choose metric by preference, else first extra; remove it from extras to avoid dup
      int bestIdx = -1, bestScore = 999;
      for (int i=0;i<(int)extras.size();++i) {
        int eq = extras[i].indexOf('=');
        if (eq<=0) continue;
        String k = extras[i].substring(0,eq);
        int sc = metric_pref(k);
        if (sc < bestScore) { bestScore = sc; bestIdx = i; }
      }
      if (bestIdx >= 0) {
        int eq = extras[bestIdx].indexOf('=');
        row.metric = extras[bestIdx].substring(eq+1);
        extras.erase(extras.begin() + bestIdx); // prevent duplicate printing
      } else if (!extras.empty()) {
        int eq = extras[0].indexOf('=');
        if (eq>0) { row.metric = extras[0].substring(eq+1); extras.erase(extras.begin()); }
      }

      row.extras = std::move(extras);
      B.rows.push_back(std::move(row));

      ++pushed;
      if (MAX_ROWS_PER_BOARD>0 && pushed>=MAX_ROWS_PER_BOARD) break;
      if ((int)B.rows.size() >= 1000) break; // hard safety
    }

    if (!B.rows.empty()) {
      std::sort(B.rows.begin(), B.rows.end(),
        [](const Row& a, const Row& b){ return rankKey(a.rank) < rankKey(b.rank); });
      boards.push_back(std::move(B));
    }
  }

  if (boards.empty()) return false;

#if defined(ESP_PLATFORM)
  curBoard = (int)(esp_random() % boards.size());
#else
  curBoard = (int)(rand() % boards.size());
#endif
  lastBoardSwitch = millis();
  scrollY = 0.f;
  freezeUntilMs = lastBoardSwitch + FREEZE_MS;
  loaded = true;
  lastModelSwitch = millis();

  if (g_dbg) Serial.printf("[INSIGNIA] %s boards=%d\n", gameTitle.c_str(), (int)boards.size());
  return true;
}

// =================== Public API ===================
void setServerBase(const String& base) { BASE = base; /* keep WORK_ROOT until re-probe needed */ }
void setFlushCacheOnBoot(bool enable) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  flushOnBoot = enable;
#else
  (void)enable;
#endif
}
void flushCacheNow() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  ensureFS(); if (!fsReady) return;
  File root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = f.path();
    if (p.startsWith(CACHE_DIR)) SPIFFS.remove(p);
  }
  if (g_dbg) Serial.println("[INSIGNIA] cache flushed now");
#endif
}
void setCacheLimits(size_t maxFiles, size_t maxBytes, uint32_t maxAgeMs) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  cacheMaxFiles = maxFiles ? maxFiles : cacheMaxFiles;
  cacheMaxBytes = maxBytes ? maxBytes : cacheMaxBytes;
  cacheMaxAgeMs = maxAgeMs ? maxAgeMs : cacheMaxAgeMs;
#endif
}

void begin(bool debug) { g_dbg = debug; resetRuntime(); }
void onAppName(const char* app) {
  String s = String(app ? app : ""); s.trim();
  if (s == curApp) return;
  curApp = s;
  resetRuntime();
  if (curApp.length()==0) return;
  if (netReady() && WORK_ROOT.length()==0) probeWorkRoot();
}

bool isActive() {
  // Only expose the Insignia screen when we have a *resolved* title and a loaded model
  return resolved && loaded;
}

// =================== Internals ===================
static void maybeResolveAndLoad() {
  const uint32_t now = millis();
  if (now - lastFetchMs < 500) return;
  lastFetchMs = now;

  if (!resolved) {
    if (!probeWorkRoot()) return;
    if (!resolveTitlePool()) return;
  }
  if (!loaded) {
    if (curTitleIdx < 0 || curTitleIdx >= (int)titlePool.size()) return;
    loadGameModel(titlePool[curTitleIdx]);
  }
}

void tick() {
  if (curApp.length()==0) return;
  maybeResolveAndLoad();
  if (!resolved || !loaded) return;

  const uint32_t now = millis();
  if (now < freezeUntilMs) return;

  if (now - lastStep >= STEP_MS) {
    lastStep = now;
    scrollY += PIXELS_PER_STEP;
  }

  // Rotate board after content clears cutoff & dwell satisfied
  if (curBoard >= 0 && curBoard < (int)boards.size()) {
    const Board& B = boards[curBoard];
    const int last_i = (int)B.rows.size() - 1;
    const int bottomBaseline = SCR_H - 2;
    const float y_last = bottomBaseline - (scrollY - last_i * (float)LINE_H);
    const float last_top = y_last - ASCENT_5x8;

    if (last_top < CONTENT_TOP && (now - lastBoardSwitch) >= BOARD_MIN_DWELL_MS) {
      // Next board (within this variant)
#if defined(ESP_PLATFORM)
      int next = (boards.size()>1) ? (int)(esp_random() % boards.size()) : curBoard;
#else
      int next = (boards.size()>1) ? (int)(rand() % boards.size()) : curBoard;
#endif
      if (boards.size()>1 && next==curBoard) next = (next+1) % boards.size();
      curBoard = next;
      scrollY = 0.f;
      lastBoardSwitch = now;
      freezeUntilMs = now + FREEZE_MS;

      // Maybe rotate to the next *variant* (e.g., NTSC <-> PAL) after a whole board cycle
      if (titlePool.size() > 1 && (now - lastModelSwitch) >= MODEL_DWELL_MS) {
        curTitleIdx = (curTitleIdx + 1) % (int)titlePool.size();
        loaded = false; // trigger load of the next variant model on next tick
        if (g_dbg) Serial.printf("[INSIGNIA] switch variant -> %s\n", titlePool[curTitleIdx].c_str());
      }
    }
  }
}

// =================== Drawing ===================
void draw(U8G2* g) {
  if (!resolved || !loaded) return;

  g->clearBuffer();

  // Header
  String head = (gameTitle.length() ? gameTitle : curApp);
  g->setFont(u8g2_font_6x12_tf);
  int w = g->getStrWidth(head.c_str());
  int x = (SCR_W - w)/2; if (x < 0) x = 0;
  g->setCursor(x, TOP_LINE_Y);
  g->print(head);
  g->drawHLine(0, RULE_Y, SCR_W);

  // Body (credits-style block)
  g->setFont(u8g2_font_5x8_tf);
  const Board& B = boards[curBoard >= 0 ? curBoard : 0];
  const int bottomBaseline = SCR_H - 2;

  for (int i=0; i<(int)B.rows.size(); ++i) {
    String L;
    const Row& r = B.rows[i];
    if (r.rank.length()) L += r.rank + ". ";
    L += r.name.length()? r.name : String("—");
    if (r.metric.length()) L += "  " + r.metric;
    for (size_t k=0;k<r.extras.size();++k) {
      L += "  \xC2\xB7 " + r.extras[k];
    }
    while (g->getStrWidth(L.c_str()) > (SCR_W - 4) && L.length() > 1) L.remove(L.length()-1);

    const float y = bottomBaseline - (scrollY - i*(float)LINE_H);
    if ((y - ASCENT_5x8) >= CONTENT_TOP && y <= (SCR_H + LINE_H)) {
      g->setCursor(2, (int)y);
      g->print(L);
    }
  }

  g->sendBuffer();
}

uint32_t recommendedHoldMs(){ return 15000; }

} // namespace Insignia
