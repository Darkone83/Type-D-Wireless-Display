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

// ---------- Forward declarations ----------
static inline bool netReady();
static String lc(const String& s);
static int romanToInt(const String& in);
static String asciiFoldKeepSpace(const String& in);
static String squeezeSpace(const String& in);
static std::vector<String> tokenize(const String& raw);
static String normKey(const String& raw);
static bool isRegionWord(const String& tok);
static String familyKeyFromLabel(const String& name);
static String familyKeyFromSlug(const String& slug);
static inline String j2s(JsonVariant v);
static bool in_list_ci(const String& s, const char* const* keys);
static int  metric_pref(const String& key);
static int  rankKey(const String& rs);
static void resetRuntime(bool keepRoot=false);
static bool resolveTitlePool();
static bool loadGameModel(const String& titleId);
static void maybeResolveAndLoad();
static void ensureFS();
static String sanitizeKey(const String& url);
static size_t dirSize(const char* dir, size_t& files);
static void pruneCache();
static bool cacheWrite(const String& url, const String& body);
static bool cacheRead(const String& url, String& out, uint32_t maxAgeMs, bool allowStaleIfNoNet);
static bool httpGetTO(const String& url, String& out, uint32_t timeoutMs);
static inline bool httpGet(const String& url, String& out);
static void buildCandidateRoots(std::vector<String>& out);
static void startProbingIfNeeded();
static bool stepProbeWorkRoot();
static int tokenOverlapScore(const std::vector<String>& qtoks, const std::vector<String>& ctoks);
static int firstTokenBoost(const std::vector<String>& qtoks, const std::vector<String>& ctoks);
static bool isGenericXLA(const std::vector<String>& ctoks);
static int tokenJaccardPenaltyShort(const String& candNorm);
static int bigramJaccardScore(const String& a, const String& b);
static int containsBonus(const String& small, const String& big);

// =================== Config / constants ===================
static bool g_dbg = true;

static String BASE = "http://darkone83.myddns.me:8080/xbox"; // CSV allowed
static String WORK_ROOT;                                     // probed root that has /data/search.json

// Screen geometry & timing
static const int SCR_W=128, SCR_H=64;
static const int TOP_LINE_Y=12, RULE_Y=14, CONTENT_TOP=RULE_Y+2;
static const int LINE_H = 9;
static const int ASCENT_5x8 = 7;
static const uint32_t STEP_MS = 40;
static const int PIXELS_PER_STEP = 1;
static const uint32_t BOARD_MIN_DWELL_MS = 3000;
static const uint32_t FREEZE_MS = 750;
static const uint32_t MODEL_DWELL_MS = 12000;

// Header aliases & options (match emulator)
static const char* RANK_KEYS[]   = {"rank","#","pos","position","place", nullptr};
static const char* NAME_KEYS[]   = {"name","player","gamertag","gamer","tag","alias","username","user","gt","account", nullptr};
static const char* PREFER_METRIC[]= {"score","points","rating","time","best time","laps","wins","value", nullptr};
static const int   MAX_ROWS_PER_BOARD = 0;  // unlimited

// Networking/prefetch tuning
static const uint32_t HTTP_TIMEOUT_MS   = 1200;
static const uint32_t PROBE_SPACING_MS  = 200;
static const uint32_t PROBE_BACKOFF_MS  = 2000;

// Match acceptance threshold
static const int MIN_ACCEPT_SCORE = 65;

// Cache policy
#if defined(ARDUINO) || defined(ESP_PLATFORM)
static const char* CACHE_DIR = "/insig";
static bool  fsReady = false;
static bool  flushOnBoot = false;
static size_t cacheMaxFiles = 32;
static size_t cacheMaxBytes = 128 * 1024;
static uint32_t cacheMaxAgeMs = 6UL*60*60*1000UL;
static const uint32_t TTL_SEARCH_MS = 6UL*60*60*1000UL;
static const uint32_t TTL_BYID_MS   = 2UL*60*1000UL;
#endif

// =================== Runtime state ===================
static String curApp;
static String gameTitle;
static bool   haveSearch = false;
static bool   resolved   = false;
static bool   loaded     = false;

// variants (NTSC / PAL etc.)
static std::vector<String> titlePool;
static int curTitleIdx = -1;
static uint32_t lastModelSwitch = 0;

struct Row {
  String rank, name;
  String metric;
  std::vector<String> extras;
};
struct Board { String name; std::vector<Row> rows; };
static std::vector<Board> boards;

static int   curBoard = -1;
static float scrollY  = 0.f;
static uint32_t lastStep = 0;
static uint32_t lastBoardSwitch = 0;
static uint32_t freezeUntilMs = 0;
static uint32_t lastFetchMs = 0;

// ---- incremental probing state ----
static std::vector<String> probeList;
static size_t probeIdx = 0;
static uint32_t nextProbeAt = 0;

// ---- diagnostics ----
struct MatchDiag {
  String id, name, slug;
  int score = 0;
  String reason;
};
static std::vector<MatchDiag> lastDiag;
static String lastQueryRaw, lastQueryNorm;

// =================== Helpers ===================
static inline bool netReady() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  return WiFi.status() == WL_CONNECTED;
#else
  return true;
#endif
}
static String lc(const String& s){ String t=s; t.toLowerCase(); return t; }

static int romanToInt(const String& in) {
  String s = lc(in);
  if (s=="i") return 1;
  if (s=="ii") return 2;
  if (s=="iii") return 3;
  if (s=="iiii" || s=="iv") return 4;
  if (s=="v") return 5;
  if (s=="vi") return 6;
  if (s=="vii") return 7;
  if (s=="viii") return 8;
  if (s=="ix") return 9;
  if (s=="x") return 10;
  return -1;
}

static String asciiFoldKeepSpace(const String& in) {
  String out; out.reserve(in.length());
  for (size_t i=0;i<in.length();++i) {
    char c = in[i];
    if (c>='A' && c<='Z') c = (char)(c-'A'+'a');
    if ((c>='a' && c<='z') || (c>='0'&&c<='9') || c==' ') { out += c; continue; }
    if (c=='&') { out += " and "; continue; }
    out += ' ';
  }
  return out;
}

static String squeezeSpace(const String& in) {
  String out; out.reserve(in.length());
  bool prevSpace=false;
  for (size_t i=0;i<in.length();++i) {
    char c=in[i];
    if (c==' ') {
      if (!prevSpace) out += c;
      prevSpace=true;
    } else { out += c; prevSpace=false; }
  }
  int s=0,e=(int)out.length();
  while (s<e && out[s]==' ') s++;
  while (e> s && out[e-1]==' ') e--;
  return (s==0 && e==(int)out.length()) ? out : out.substring(s,e);
}

static std::vector<String> tokenize(const String& raw) {
  String s = lc(raw);
  if (s.length()>1 && s[0]=='x' && ((s[1]>='a'&&s[1]<='z') || (s[1]>='0'&&s[1]<='9'))) s = s.substring(1);
  if (s.startsWith("the ")) s = s.substring(4);
  s = asciiFoldKeepSpace(s);
  s = squeezeSpace(s);

  std::vector<String> toks;
  int i=0, n=(int)s.length();
  while (i<n) {
    while (i<n && s[i]==' ') i++;
    int j=i; while (j<n && s[j]!=' ') j++;
    if (j>i) {
      String tok = s.substring(i,j);
      int r = romanToInt(tok);
      if (r>0) tok = String(r);
      toks.push_back(tok);
    }
    i=j+1;
  }
  return toks;
}

static String normKey(const String& raw) {
  auto toks = tokenize(raw);
  String out; out.reserve(raw.length());
  for (size_t k=0;k<toks.size();++k) out += toks[k];
  return out;
}

static bool isRegionWord(const String& tok) {
  String t = tok; t.toLowerCase();
  if (t.endsWith(",")) t.remove(t.length()-1);
  return (t=="ntsc" || t=="pal" || t=="usa" || t=="us" ||
          t=="japan"|| t=="jpn" || t=="germany" || t=="de" ||
          t=="europe"|| t=="eu"  || t=="asia"   || t=="kor" ||
          t=="korea" || t=="au"  || t=="australia");
}

static String familyKeyFromLabel(const String& name) {
  String s = name;
  int open = s.lastIndexOf('(');
  int close = s.lastIndexOf(')');
  if (open >= 0 && close > open && close == s.length()-1) {
    String inside = s.substring(open+1, close);
    std::vector<String> toks;
    int i=0, n=(int)inside.length();
    while (i<n) {
      while (i<n && (inside[i]==' ' || inside[i]==',')) i++;
      int j=i; while (j<n && inside[j]!=',' && inside[j]!=' ') j++;
      if (j>i) toks.emplace_back(inside.substring(i,j));
      i=j;
    }
    bool allRegion = !toks.empty();
    for (auto& t : toks) if (!isRegionWord(t)) { allRegion=false; break; }
    if (allRegion) s = s.substring(0, open);
  }
  s.trim();
  return normKey(s);
}

static String familyKeyFromSlug(const String& slug) {
  String s = slug; s.toLowerCase();
  const char* suf[] = {
    "-ntsc","-pal","-usa","-japan","-jpn","-germany","-eu","-europe","-asia","-kor","-korea", nullptr
  };
  for (int i=0; suf[i]; ++i) {
    const int L = (int)strlen(suf[i]);
    if ((int)s.length() > L && s.endsWith(suf[i])) { s.remove(s.length()-L); break; }
  }
  String spaced = s; spaced.replace("-", " ");
  return normKey(spaced);
}

static inline String j2s(JsonVariant v) {
  if (v.isNull()) return String();
  const char* p = v.as<const char*>();
  return p ? String(p) : String(v.as<String>());
}

static bool in_list_ci(const String& s, const char* const* keys){
  String L = s; L.toLowerCase();
  for (int i=0; keys[i]; ++i) if (L == keys[i]) return true;
  return false;
}
static int metric_pref(const String& key){
  static const char* PREFER_METRIC_LOCAL[] = {"score","points","rating","time","best time","laps","wins","value", nullptr};
  String L = key; L.toLowerCase();
  for (int i=0; PREFER_METRIC_LOCAL[i]; ++i) if (L == PREFER_METRIC_LOCAL[i]) return i;
  return 100;
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

// =================== SPIFFS cache (ESP only) ===================
#if defined(ARDUINO) || defined(ESP_PLATFORM)
static void ensureFS() {
  if (!fsReady) {
    fsReady = SPIFFS.begin(true);
    if (g_dbg) Serial.printf("[INSIGNIA] SPIFFS %s\n", fsReady ? "mounted" : "MOUNT FAIL");
    if (fsReady && flushOnBoot) {
      File root = SPIFFS.open("/");
      for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        String p = f.path();
        if (p.startsWith(CACHE_DIR)) SPIFFS.remove(p);
      }
      if (g_dbg) Serial.println("[INSIGNIA] cache flushed on boot");
      flushOnBoot = false;
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
  std::vector<std::pair<String, time_t>> entries;
  File root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = f.path();
    if (!p.startsWith(CACHE_DIR)) continue;
    entries.emplace_back(p, f.getLastWrite());
  }
  time_t nowt = time(nullptr);
  for (auto& e : entries) {
    File f = SPIFFS.open(e.first);
    if (!f) continue;
    bool tooOld = (nowt>0 && e.second>0) && ((uint32_t)((nowt - e.second)*1000UL) > cacheMaxAgeMs);
    f.close();
    if (tooOld) SPIFFS.remove(e.first);
  }
  size_t files=0, bytes = dirSize(CACHE_DIR, files);
  if (files <= cacheMaxFiles && bytes <= cacheMaxBytes) return;

  entries.clear();
  root = SPIFFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = f.path();
    if (!p.startsWith(CACHE_DIR)) continue;
    entries.emplace_back(p, f.getLastWrite());
  }
  std::sort(entries.begin(), entries.end(),
            [](auto& a, auto& b){ return a.second < b.second; });

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
static bool httpGetTO(const String& url, String& out, uint32_t timeoutMs) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}
static inline bool httpGet(const String& url, String& out) {
  return httpGetTO(url, out, HTTP_TIMEOUT_MS);
}

// =================== Root probing ===================
static void buildCandidateRoots(std::vector<String>& out) {
  out.clear();
  String csv = BASE; csv.trim();
  if (!csv.length()) return;

  auto add = [&](const String& s){
    String t = s; while (t.endsWith("/")) t.remove(t.length()-1);
    if (t.length() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
  };

  int start = 0;
  while (start < (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    String b = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
    start = (comma < 0) ? csv.length() : (comma + 1);
    b.trim();
    if (!b.length()) continue;

    add(b);
    if (b.endsWith("/data")) { String p=b; p.remove(p.length()-5); add(p); }
    add(b + "/xbox");
    add(b + "/xbox/data");
  }
}

static void startProbingIfNeeded() {
  if (!probeList.empty() || WORK_ROOT.length()) return;
  buildCandidateRoots(probeList);
  probeIdx = 0;
  nextProbeAt = 0;
  if (g_dbg && !probeList.empty()) {
    Serial.printf("[INSIGNIA] probe %u candidates\n", (unsigned)probeList.size());
  }
}

static bool stepProbeWorkRoot() {
  if (WORK_ROOT.length()) return true;
  if (!netReady()) return false;
  if (probeList.empty()) startProbingIfNeeded();

  const uint32_t now = millis();
  if (now < nextProbeAt) return false;
  nextProbeAt = now + PROBE_SPACING_MS;

  if (probeIdx >= probeList.size()) {
    nextProbeAt = now + PROBE_BACKOFF_MS;
    probeIdx = 0;
    return false;
  }

  const String r = probeList[probeIdx++];
  const String url = r + "/data/search.json";
  String body;

#if defined(ARDUINO) || defined(ESP_PLATFORM)
  if (cacheRead(url, body, TTL_SEARCH_MS, true)) {
    DynamicJsonDocument tmp(512);
    if (!deserializeJson(tmp, body)) {
      WORK_ROOT = r;
      if (g_dbg) Serial.printf("[INSIGNIA] WORK_ROOT via cache: %s\n", r.c_str());
      return true;
    }
  }
#endif

  if (httpGet(url, body)) {
    #if defined(ARDUINO) || defined(ESP_PLATFORM)
      cacheWrite(url, body);
    #endif
    DynamicJsonDocument tmp(512);
    if (!deserializeJson(tmp, body)) {
      WORK_ROOT = r;
      if (g_dbg) Serial.printf("[INSIGNIA] WORK_ROOT via net: %s\n", r.c_str());
      return true;
    }
  }
  return false;
}

// ===== scoring helpers =====
static int tokenOverlapScore(const std::vector<String>& qtoks, const std::vector<String>& ctoks) {
  if (qtoks.empty() || ctoks.empty()) return 0;
  int matches = 0;
  for (auto& q : qtoks) {
    for (auto& c : ctoks) {
      if (q == c) { matches++; break; }
    }
  }
  int s = matches * 12; if (s > 60) s = 60;
  return s;
}
static int firstTokenBoost(const std::vector<String>& qtoks, const std::vector<String>& ctoks) {
  if (qtoks.empty() || ctoks.empty()) return 0;
  return (qtoks.front() == ctoks.front()) ? 25 : 0;
}
static bool isGenericXLA(const std::vector<String>& ctoks) {
  const char* G[] = {"xbox","live","arcade", nullptr};
  if (ctoks.empty()) return false;
  for (auto& t : ctoks) {
    bool ok=false; for (int i=0; G[i]; ++i) if (t==G[i]) { ok=true; break; }
    if (!ok) return false;
  }
  return true;
}
static int tokenJaccardPenaltyShort(const String& candNorm) {
  if ((int)candNorm.length() <= 6) return -20;
  return 0;
}
static int bigramJaccardScore(const String& a, const String& b) {
  if (!a.length() || !b.length()) return 0;
  auto grams = [](const String& s)->std::vector<String>{
    std::vector<String> g; g.reserve(s.length());
    for (size_t i=1;i<s.length();++i) g.emplace_back(s.substring(i-1,i+1));
    std::sort(g.begin(), g.end()); g.erase(std::unique(g.begin(), g.end()), g.end());
    return g;
  };
  auto A = grams(a), B = grams(b);
  size_t i=0,j=0, inter=0;
  while (i<A.size() && j<B.size()) {
    if (A[i]==B[j]) { inter++; i++; j++; }
    else if (A[i]<B[j]) i++; else j++;
  }
  size_t uni = A.size() + B.size() - inter;
  if (uni==0) return 0;
  float jacc = (float)inter / (float)uni;
  int score = (int)(jacc * 70.0f);
  if (score<0) score=0; if (score>70) score=70;
  return score;
}
static int containsBonus(const String& small, const String& big) {
  if (!small.length() || !big.length()) return 0;
  if (big.indexOf(small) >= 0) {
    int bonus = 15;
    if ((int)small.length() >= 5) bonus = 18;
    if ((int)small.length() >= 8) bonus = 22;
    if ((int)small.length() >= 12) bonus = 25;
    return bonus;
  }
  return 0;
}

// ---------- tiny JSON scanner for search.json ----------
static inline int find_unescaped_quote(const String& s, int pos) {
  int i = pos+1;
  while (i < (int)s.length()) {
    char c = s[i];
    if (c == '\\') { i += 2; continue; }
    if (c == '"') return i;
    ++i;
  }
  return -1;
}
static inline bool extract_str_field(const String& obj, const char* key, String& out) {
  String pat = String("\"") + key + String("\"");
  int k = obj.indexOf(pat);
  if (k < 0) return false;
  int colon = obj.indexOf(':', k + pat.length());
  if (colon < 0) return false;
  int q1 = obj.indexOf('"', colon+1);
  if (q1 < 0) return false;
  int q2 = find_unescaped_quote(obj, q1);
  if (q2 < 0) return false;
  out = obj.substring(q1+1, q2);
  return true;
}

// =================== Resolve by App → build title pool ===================
static bool resolveTitlePool() {
  if (!netReady() && WORK_ROOT.length()==0) return false;
  if (WORK_ROOT.length()==0) return false;

  if (!curApp.length()) return false;                 // guard: need a query
  lastQueryRaw  = curApp;
  lastQueryNorm = normKey(curApp);
  if (!lastQueryNorm.length()) return false;          // guard: normalized empty

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

  auto qToks = tokenize(curApp);

  // ------- pass 1: find best candidate with strict overlap gate -------
  struct Best { String id,name,nlc,slug,fam; int score=0; String reason; };
  Best best;

  struct DItem { String id,name,slug; int score; String reason; };
  DItem diags[10]; int diagN=0;

  int idx = body.indexOf('{');
  while (idx >= 0) {
    int end = body.indexOf('}', idx);
    if (end < 0) break;
    String obj = body.substring(idx, end+1);

    String id, name, nlc, slug;
    if (!extract_str_field(obj, "title_id", id)) { idx = body.indexOf('{', end+1); continue; }
    extract_str_field(obj, "name", name);
    extract_str_field(obj, "name_lc", nlc);
    extract_str_field(obj, "slug", slug);

    String nName = normKey(name);
    String nSlug = normKey(slug);
    auto   tName = tokenize(name);
    auto   tSlug = tokenize(slug);

    int score = 0; String reason;

    // Strong exacts
    if (lc(name) == lc(curApp)) { score = 100; reason = "exact name"; }
    else if (nlc.length() && nlc == lc(curApp)) { score = 98; reason = "exact name_lc"; }
    else if (lc(slug) == lc(curApp)) { score = 95; reason = "exact slug"; }
    else if (nName == lastQueryNorm) { score = 93; reason = "norm(name)"; }
    else if (nSlug == lastQueryNorm) { score = 91; reason = "norm(slug)"; }
    else {
      int stName = tokenOverlapScore(qToks, tName) + firstTokenBoost(qToks, tName);
      int stSlug = tokenOverlapScore(qToks, tSlug) + firstTokenBoost(qToks, tSlug);
      int sb1 = bigramJaccardScore(lastQueryNorm, nName);
      int sb2 = bigramJaccardScore(lastQueryNorm, nSlug);
      int sc1 = containsBonus(lastQueryNorm, nName);
      int sc2 = containsBonus(lastQueryNorm, nSlug);
      int sc3 = containsBonus(nName, lastQueryNorm);
      int sc4 = containsBonus(nSlug, lastQueryNorm);
      score = std::max(std::max(stName, stSlug),
                       std::max(std::max(sb1, sb2),
                                std::max(std::max(sc1, sc2), std::max(sc3, sc4))));

      // Penalize ultra-short candidate when head-token not aligned
      if (firstTokenBoost(qToks, tName) == 0 && firstTokenBoost(qToks, tSlug) == 0) {
        score += tokenJaccardPenaltyShort(nName);
      }
      // De-prefer generic "Xbox Live Arcade" etc unless user typed "xbox"
      if (isGenericXLA(tName)) {
        if (qToks.empty() || qToks.front() != "xbox") score -= 35;
      }
      if (score < 0) score = 0;
    }

    // ---- HARD GATE: require some *semantic* overlap, not just bigrams ----
    bool tokenOverlap = false;
    if (!qToks.empty()) {
      for (auto& q : qToks) {
        for (auto& c : tName) if (q==c) { tokenOverlap=true; break; }
        if (tokenOverlap) break;
        for (auto& c : tSlug) if (q==c) { tokenOverlap=true; break; }
        if (tokenOverlap) break;
      }
    }
    bool containsEither =
      (nName.indexOf(lastQueryNorm)>=0) || (nSlug.indexOf(lastQueryNorm)>=0) ||
      (lastQueryNorm.indexOf(nName)>=0) || (lastQueryNorm.indexOf(nSlug)>=0);

    if (!(tokenOverlap || containsEither)) {
      // If there is no exact token overlap and neither side contains the other,
      // we treat it as unrelated (prevents XBMC4Gamers → Pirates, etc.)
      score = 0;
      reason = "";
    }

    // record diagnostics (bounded)
    if (score > 0 && diagN < 10) {
      diags[diagN++] = DItem{id,name,slug,score,reason};
    }

    // choose best (tie-breakers)
    auto better = [&](const Best& A, int sc, const String& nNm, const std::vector<String>& tNm)->bool{
      if (sc > A.score) return true;
      if (sc < A.score) return false;
      int da = abs((int)nNm.length() - (int)lastQueryNorm.length());
      int db = abs((int)normKey(A.name).length() - (int)lastQueryNorm.length());
      if (da != db) return da < db;
      auto qT = tokenize(lastQueryRaw);
      bool af = (!qT.empty() && !tNm.empty() && qT.front()==tNm.front());
      bool bf = (!qT.empty() && !tokenize(A.name).empty() && qT.front()==tokenize(A.name).front());
      if (af != bf) return af;
      return name.length() < A.name.length();
    };

    if (score >= MIN_ACCEPT_SCORE && (best.id.length()==0 || better(best, score, nName, tName))) {
      best.id=id; best.name=name; best.nlc=nlc; best.slug=slug; best.score=score; best.reason=reason;
      String fam = familyKeyFromLabel(name);
      if (!fam.length()) fam = familyKeyFromSlug(slug);
      best.fam = fam;
    }

    idx = body.indexOf('{', end+1);
  }

  if (best.id.length()==0 || best.score < MIN_ACCEPT_SCORE) {
    if (g_dbg) {
      Serial.printf("[INSIGNIA] No acceptable match for app='%s' norm='%s' (root=%s)\n",
                    curApp.c_str(), lastQueryNorm.c_str(), WORK_ROOT.c_str());
      for (int i=0;i<diagN;i++) {
        Serial.printf("  • %-3d  %s  (slug=%s, id=%s)  [%s]\n",
                      diags[i].score, diags[i].name.c_str(), diags[i].slug.c_str(), diags[i].id.c_str(), diags[i].reason.c_str());
      }
    }
    return false;
  }

  // ------- pass 2: collect family mates -------
  titlePool.clear();
  {
    int idx2 = body.indexOf('{');
    while (idx2 >= 0) {
      int end = body.indexOf('}', idx2);
      if (end < 0) break;
      String obj = body.substring(idx2, end+1);

      String id, name, slug;
      if (!extract_str_field(obj, "title_id", id)) { idx2 = body.indexOf('{', end+1); continue; }
      extract_str_field(obj, "name", name);
      extract_str_field(obj, "slug", slug);

      String fam = familyKeyFromLabel(name);
      if (!fam.length()) fam = familyKeyFromSlug(slug);

      if (fam == best.fam && id.length()) {
        if (std::find(titlePool.begin(), titlePool.end(), id) == titlePool.end()) {
          titlePool.push_back(id);
        }
      }

      idx2 = body.indexOf('{', end+1);
    }
  }

  if (titlePool.empty()) titlePool.push_back(best.id);

#if defined(ESP_PLATFORM)
  curTitleIdx = (int)(esp_random() % titlePool.size());
#else
  curTitleIdx = (int)(rand() % titlePool.size());
#endif

  haveSearch = true;
  resolved   = true;

  lastDiag.clear();
  for (int i=0;i<diagN;i++) {
    MatchDiag d; d.id=diags[i].id; d.name=diags[i].name; d.slug=diags[i].slug; d.score=diags[i].score; d.reason=diags[i].reason;
    lastDiag.push_back(std::move(d));
  }

  if (g_dbg) {
    Serial.printf("[INSIGNIA] pool size=%d (family='%s') query='%s' norm='%s' best='%s' score=%d\n",
                  (int)titlePool.size(), best.fam.c_str(), curApp.c_str(), lastQueryNorm.c_str(),
                  best.name.c_str(), best.score);
  }
  return true;
}

// =================== Load one variant model ===================
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

  DynamicJsonDocument doc(1 << 16); // ~64KB
  DeserializationError derr = deserializeJson(doc, body);
  if (derr) {
    if (g_dbg) Serial.printf("[INSIGNIA] JSON parse fail (%s) for %s\n", derr.c_str(), titleId.c_str());
    return false;
  }

  boards.clear(); curBoard=-1; gameTitle="";
  gameTitle = j2s(doc["game_title"]);

  JsonArray sbs = doc["scoreboards"].as<JsonArray>();
  if (sbs.isNull()) {
    if (g_dbg) Serial.printf("[INSIGNIA] no scoreboards array for %s\n", titleId.c_str());
    return false;
  }

  for (JsonVariant vSB : sbs) {
    if (!vSB.is<JsonObject>()) continue;
    JsonObject sb = vSB.as<JsonObject>();
    Board B; B.name = j2s(sb["name"]); if (!B.name.length()) B.name = "default";

    // columns
    std::vector<String> cols;
    if (sb["columns"].is<JsonArray>()) {
      for (JsonVariant v : sb["columns"].as<JsonArray>()) cols.emplace_back(j2s(v));
    }

    JsonArray rowsA = sb["rows"].as<JsonArray>();
    if (rowsA.isNull()) continue;

    if (cols.empty() && rowsA.size()>0 && rowsA[0].is<JsonObject>()) {
      JsonObject first = rowsA[0].as<JsonObject>();
      for (JsonPair kv : first) cols.emplace_back(String(kv.key().c_str()));
    }

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
          return j2s(r[cols[i]]);
        };

        row.rank = (rankIdx>=0) ? val_by_col(rankIdx) : String();
        row.name = (nameIdx>=0) ? val_by_col(nameIdx) : String();

        if (row.rank.isEmpty()) {
          for (JsonPair kv : r) { if (in_list_ci(kv.key().c_str(), RANK_KEYS)) { row.rank = j2s(kv.value()); break; } }
        }
        if (row.name.isEmpty()) {
          for (JsonPair kv : r) { if (in_list_ci(kv.key().c_str(), NAME_KEYS)) { row.name = j2s(kv.value()); break; } }
        }
        if (row.rank.isEmpty()) row.rank = String((int)B.rows.size() + 1);

        for (size_t i=0;i<cols.size();++i) {
          if ((int)i==rankIdx || (int)i==nameIdx) continue;
          String v = val_by_col((int)i);
          if (v.length()) extras.emplace_back(cols[i] + "=" + v);
        }
        for (JsonPair kv : r) {
          String k = String(kv.key().c_str());
          if (!k.length() || isDeclared(k)) continue;
          String v = j2s(kv.value());
          if (v.length()) extras.emplace_back(k + "=" + v);
        }

      } else if (rv.is<JsonArray>()) {
        JsonArray arr = rv.as<JsonArray>();
        auto val_at = [&](int i)->String {
          if (i < 0 || i >= (int)arr.size()) return String();
          JsonVariant v = arr[i]; return j2s(v);
        };

        row.rank = (rankIdx>=0) ? val_at(rankIdx) : String((int)B.rows.size() + 1);
        row.name = (nameIdx>=0) ? val_at(nameIdx) : String();

        for (size_t i=0;i<cols.size();++i) {
          if ((int)i==rankIdx || (int)i==nameIdx) continue;
          String v = val_at((int)i);
          if (v.length()) extras.emplace_back(cols[i] + "=" + v);
        }

      } else {
        row.rank = String((int)B.rows.size() + 1);
        row.name = j2s(rv);
      }

      // strip Rank/Name-like from extras
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

      // choose metric
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
        extras.erase(extras.begin() + bestIdx);
      } else if (!extras.empty()) {
        int eq = extras[0].indexOf('=');
        if (eq>0) { row.metric = extras[0].substring(eq+1); extras.erase(extras.begin()); }
      }

      row.extras = std::move(extras);
      B.rows.push_back(std::move(row));

      ++pushed;
      if (MAX_ROWS_PER_BOARD>0 && pushed>=MAX_ROWS_PER_BOARD) break;
      if ((int)B.rows.size() >= 1000) break;
    }

    if (!B.rows.empty()) {
      std::sort(B.rows.begin(), B.rows.end(),
        [](const Row& a, const Row& b){ return rankKey(a.rank) < rankKey(b.rank); });
      boards.push_back(std::move(B));
    }
  }

  if (boards.empty()) {
    if (g_dbg) Serial.printf("[INSIGNIA] %s parsed but 0 usable boards\n", titleId.c_str());
    return false;
  }

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
void setServerBase(const String& base) { BASE = base; }
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

static void resetRuntime(bool keepRoot) {
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

  probeList.clear();
  probeIdx = 0;
  nextProbeAt = 0;

  lastDiag.clear();
  lastQueryRaw = "";
  lastQueryNorm = "";
}

void begin(bool debug) {
  g_dbg = debug;
  resetRuntime();
  if (netReady()) startProbingIfNeeded();
}

void onAppName(const char* app) {
  String s = String(app ? app : ""); s.trim();
  if (s == curApp) return;
  curApp = s;
  resetRuntime();
  if (curApp.length()==0) return;
  if (netReady()) startProbingIfNeeded();
}

bool isActive() { return (curApp.length()>0) && resolved && loaded; }

void dumpSearchDebug() {
  if (!g_dbg) return;
  Serial.printf("[INSIGNIA] Search debug: app='%s' norm='%s' root='%s'\n",
                lastQueryRaw.c_str(), lastQueryNorm.c_str(), WORK_ROOT.c_str());
  if (lastDiag.empty()) { Serial.println("  (no candidates cached)"); return; }
  for (auto& d : lastDiag) {
    Serial.printf("  • %-3d  %s  (slug=%s, id=%s)  [%s]\n",
                  d.score, d.name.c_str(), d.slug.c_str(), d.id.c_str(), d.reason.c_str());
  }
}

// =================== Internals ===================
static void maybeResolveAndLoad() {
  const uint32_t now = millis();
  if (now - lastFetchMs < 100) return;
  lastFetchMs = now;

  if (!WORK_ROOT.length()) { stepProbeWorkRoot(); return; }
  if (!resolved) { resolveTitlePool(); return; }
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

  if (curBoard >= 0 && curBoard < (int)boards.size()) {
    const Board& B = boards[curBoard];
    const int last_i = (int)B.rows.size() - 1;
    const int bottomBaseline = SCR_H - 2;
    const float y_last = bottomBaseline - (scrollY - last_i * (float)LINE_H);
    const float last_top = y_last - ASCENT_5x8;

    const int CONTENT_BODY_TOP = CONTENT_TOP + LINE_H;

    if (last_top < CONTENT_BODY_TOP && (now - lastBoardSwitch) >= BOARD_MIN_DWELL_MS) {
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

      if (titlePool.size() > 1 && (now - lastModelSwitch) >= MODEL_DWELL_MS) {
        curTitleIdx = (curTitleIdx + 1) % (int)titlePool.size();
        loaded = false;
        if (g_dbg) Serial.printf("[INSIGNIA] switch variant -> %s\n", titlePool[curTitleIdx].c_str());
      }
    }
  }
}

// =================== Drawing ===================
void draw(U8G2* g) {
  if (!resolved || !loaded) return;

  g->clearBuffer();

  String head = (gameTitle.length() ? gameTitle : curApp);
  g->setFont(u8g2_font_6x12_tf);
  int w = g->getStrWidth(head.c_str());
  int x = (SCR_W - w)/2; if (x < 0) x = 0;
  g->setCursor(x, TOP_LINE_Y);
  g->print(head);
  g->drawHLine(0, RULE_Y, SCR_W);

  g->setFont(u8g2_font_5x8_tf);
  const Board& B = boards[curBoard >= 0 ? curBoard : 0];
  const int bottomBaseline = SCR_H - 2;
  const int CONTENT_BODY_TOP = CONTENT_TOP + LINE_H;

  if (B.name.length()) {
    String label = B.name;
    while (g->getStrWidth(label.c_str()) > (SCR_W - 4) && label.length() > 1) label.remove(label.length()-1);
    g->setCursor(2, CONTENT_TOP + ASCENT_5x8);
    g->print(label);
  }

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
    if ((y - ASCENT_5x8) >= CONTENT_BODY_TOP && y <= (SCR_H + LINE_H)) {
      g->setCursor(2, (int)y);
      g->print(L);
    }
  }

  g->sendBuffer();
}

uint32_t recommendedHoldMs(){ return 15000; }

} // namespace Insignia
