#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "us2066_view.h"
#include "udp_typed.h"

// ================= helpers (same logic as display.cpp) =================
static bool av_is_hd(int v){ v &= 0xFF; return (v==0x01)||(v==0x02)||((v&0x0E)==0x0A); }

static const char* avLabelFromRaw(int v){
  v &= 0xFF;
  switch (v){
    case 0x00: return "SCART";
    case 0x01: return "HDTV (Component)";
    case 0x02: return "VGA";
    case 0x03: return "RFU";
    case 0x04: return "Advanced (S-Video)";
    case 0x06: return "Standard (Composite)";
    case 0x07: return "Missing/Disconnected";
  }
  switch (v & 0x0E){
    case 0x00: return "None/Disconnected";
    case 0x02: return "Standard (Composite)";
    case 0x06: return "Advanced (S-Video)";
    case 0x0A: return "HDTV (Component)";
    case 0x0E: return "SCART";
  }
  return "Unknown";
}

static String sdSystemFromH(int h){ return (h >= 570) ? "PAL" : "NTSC"; }

static String modeFromRes(int w,int h,int avraw){
  if (w >= 1900 && h == 1080) return "1080i";
  if (w == 1280 && h == 720)  return "720p";
  if ((w == 640 || w == 704 || w == 720) && h == 480) return av_is_hd(avraw) ? "480p" : "480i";
  if (w == 720 && h == 576)   return av_is_hd(avraw) ? "576p" : "576i";
  return "";
}

static const char* encLabelFromRaw(int v){
  switch (v & 0xFF){
    case 0x45: return "Conexant";
    case 0x6A: return "Focus";
    case 0x70: return "Xcalibur";
    default:   return "Unknown";
  }
}

static const char* xboxVerFromCode(int v){
  switch (v & 0xFF){
    case 0: return "v1.0"; case 1: return "v1.1"; case 2: return "v1.2";
    case 3: return "v1.3"; case 4: return "v1.4"; case 5: return "v1.5";
    case 6: return "v1.6";
    default: return "Not reported";
  }
}

static bool parseSerialYWWFF(const String& s,int& Y,int& W,int& F){
  int len=s.length(), run=0, end=-1;
  for (int i=len-1;i>=0;--i){
    if (isdigit((unsigned char)s[i])) { if (++run==5) { end=i+5; break; } }
    else run=0;
  }
  if (end<0) return false;
  int start=end-5;
  Y=2000+(s[start]-'0');
  W=(s[start+1]-'0')*10+(s[start+2]-'0');
  F=(s[start+3]-'0')*10+(s[start+4]-'0');
  return (W>=1 && W<=53);
}

static String versionFromYearWeek(int y,int w){
  if (y==2001) return "v1.0";
  if (y==2002){ if (w<=43) return "v1.0"; if (w<=47) return "v1.1"; return "v1.2"; }
  if (y==2003){ if (w<=8)  return "v1.2"; if (w<=30) return "v1.3"; return "v1.4"; }
  if (y==2004){ if (w<=10) return "v1.4"; if (w<=37) return "v1.6"; return "v1.6b"; }
  if (y>=2005) return "v1.6b";
  return "Not reported";
}

static String guessFromSerialAndEncoder(int encRaw,const String& serial){
  int y=0,w=0,f=0; bool ok=parseSerialYWWFF(serial,y,w,f);
  int enc=(encRaw&0xFF);
  auto encSuggest=[&]()->String{ if(enc==0x70) return "v1.6"; if(enc==0x6A) return "v1.4"; if(enc==0x45) return "v1.0-1.3"; return ""; };
  if (!ok){ String e=encSuggest(); return e.length()?e:"Not reported"; }
  if (f==3) return "v1.0";
  if (f==2) return (y<2002 || (y==2002 && w<44)) ? "v1.0" : "v1.1";
  String yw=versionFromYearWeek(y,w);
  if (enc==0x70) return (y>=2004 && w>=38) ? "v1.6b" : "v1.6";
  if (enc==0x6A){ if (yw.startsWith("v1.0")||yw.startsWith("v1.1")||yw.startsWith("v1.2")||yw.startsWith("v1.3")) return "v1.4"; }
  if (enc==0x45){ if (yw.startsWith("v1.4")||yw.startsWith("v1.6")) return "v1.3"; }
  return yw;
}

// ---------- endian helpers ----------
static inline int32_t bswap32(int32_t v){
  uint32_t x = (uint32_t)v;
  x = (x>>24) | ((x>>8)&0x0000FF00u) | ((x<<8)&0x00FF0000u) | (x<<24);
  return (int32_t)x;
}
static inline bool sane_fan(int32_t v){ return v>=0 && v<=100; }
static inline bool sane_temp(int32_t v){ return v>-50 && v<120; }
static inline bool sane_resw(int32_t v){ return v>100 && v<4097; }
static inline bool sane_resh(int32_t v){ return v>100 && v<2161; }
static inline bool sane_av (int32_t v){ v&=0xFF; return v==0x00||v==0x01||v==0x02||v==0x03||v==0x04||v==0x06||v==0x07||((v&0x0E)==0x0A)||((v&0x0E)==0x0E)||((v&0x0E)==0x06)||((v&0x0E)==0x02); }
static inline bool sane_xb (int32_t v){ return v>=0 && v<=6; }
static inline bool sane_enc(int32_t v){ v&=0xFF; return v==0x45||v==0x6A||v==0x70; }

// ================= Weather (SSD1309-style, local to US2066) =============
static struct {
  bool     enabled = false;
  char     units   = 'F';       // 'F' or 'C'
  double   lat     = NAN;
  double   lon     = NAN;
  uint32_t refresh_ms = 10UL*60UL*1000UL;
  char     place[32] = {0};

  // runtime
  bool     ok = false;
  float    temp = NAN;          // already in the selected units
  int      rh = -1;             // %
  int      code = -1;
  uint32_t last_fetch = 0;
} W;

static void loadWeatherPrefsOnce(){
  static bool loaded=false; if (loaded) return;
  Preferences p;
  if (!p.begin("weather", true)) return;
  W.enabled = p.getBool("enabled", false);
  String u  = p.getString("units", "F");
  W.units   = (u.length() && (u[0]=='C'||u[0]=='c')) ? 'C' : 'F';
  W.lat     = p.getDouble("lat", NAN);
  W.lon     = p.getDouble("lon", NAN);
  int refMin = p.getInt("refresh", 10);
  if (refMin < 1) refMin = 1; if (refMin > 120) refMin = 120;
  W.refresh_ms = (uint32_t)refMin * 60UL * 1000UL;
  String nm = p.getString("name", "");
  nm.toCharArray(W.place, sizeof(W.place));
  p.end();
  loaded = true;
}

static const char* labelForCode(int wmo){
  // minimal table (common codes). Fallback "—" keeps code size small.
  switch (wmo){
    case 0: return "Clear";
    case 1: case 2: case 3: return "Partly cloudy";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 61: case 63: case 65: return "Rain";
    case 71: case 73: case 75: return "Snow";
    case 80: case 81: case 82: return "Showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "T-storm hail";
  }
  return "—";
}

static bool fetchWeatherNow(){
  if (!W.enabled) return false;
  if (!WiFi.isConnected()) return false;
  if (isnan(W.lat) || isnan(W.lon)) return false;

  // Build Open-Meteo URL (same as SSD1309)
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(W.lat, 5) +
               "&longitude=" + String(W.lon, 5) +
               "&current=temperature_2m,weather_code,relative_humidity_2m";
  if (W.units == 'F') url += "&temperature_unit=fahrenheit";
  else                url += "&temperature_unit=celsius";

  WiFiClientSecure ssl; ssl.setInsecure();
  HTTPClient http; http.setTimeout(4000);
  if (!http.begin(ssl, url)) return false;
  int code = http.GET();
  if (code != 200){ http.end(); return false; }
  String body = http.getString();
  http.end();

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;

  W.temp = cur["temperature_2m"] | NAN;
  W.rh   = cur["relative_humidity_2m"] | -1;
  W.code = cur["weather_code"] | -1;
  W.ok   = !isnan(W.temp) && (W.code >= 0);
  W.last_fetch = millis();
  return W.ok;
}

// ======================== Class =====================================
US2066View::US2066View(){}

bool US2066View::attach(US2066* dev){
  d_=dev;
  if (!d_) return false;
  if (!d_->ping()) return false;
  d_->clear();
  d_->displayOn(true);
  // weather prefs read once (does not fetch yet)
  loadWeatherPrefsOnce();
  return true;
}

void US2066View::setStatus(const US2066_Status& s){ st_=s; }

void US2066View::setPagePeriod(uint32_t ms){ if(ms) page_ms_ = ms; }
void US2066View::forcePage(uint8_t idx){ page_=idx%4; last_page_ms_=millis(); }
void US2066View::clear(){ if(d_) d_->clear(); }

void US2066View::splash(const char* l0,const char* l1,const char* l2,const char* l3){
  if (!d_) return;
  d_->clear();
  d_->writeLine(0, l0?l0:"", true);
  d_->writeLine(1, l1?l1:"", true);
  d_->writeLine(2, l2?l2:"", true);
  d_->writeLine(3, l3?l3:"", true);
}

void US2066View::loop(){
  if (!d_) return;

  // Drive local weather fetch cadence (only if enabled + coords present)
  loadWeatherPrefsOnce();
  if (W.enabled && !isnan(W.lat) && !isnan(W.lon)) {
    uint32_t now = millis();
    const bool stale = (now - W.last_fetch) >= W.refresh_ms;
    if (stale || !W.ok) {
      fetchWeatherNow(); // non-blocking enough (4s timeout); small JSON
    }
  }

  drainUdpAndMergeStatus();

  static String ipStr;
  if (WiFi.isConnected()){
    ipStr = WiFi.localIP().toString();
    st_.ip = ipStr.c_str();
    st_.rssi_dbm = WiFi.RSSI();
  } else {
    st_.ip = nullptr;
    st_.rssi_dbm = INT32_MIN;
  }
  st_.uptime_ms = millis();

  uint32_t now=millis();
  if (now - last_page_ms_ >= page_ms_){
    uint8_t next=(page_+1)%4;

    // Skip Weather page if disabled or never fetched ok
    if (next==3 && !(W.enabled && W.ok)) next=0;

    page_=next;
    last_page_ms_=now;
  }

  switch (page_){
    case 0: drawPageA(); break;
    case 1: drawPageB(); break;
    case 2: drawPage3(); break;
    default: drawPage4(); break; // Weather
  }
}

// ================= Ingest A/B/EE (text + binary, robust) =================
void US2066View::drainUdpAndMergeStatus(){
  struct MainCache { bool have=false; int32_t fan=INT32_MIN,cpu=INT32_MIN,amb=INT32_MIN; char app[33]={0}; } MAIN;
  struct ExtCache  { bool have=false; int32_t tray=-1,av=-1,pic=-1,xb=-1,w=0,h=0,enc=-1; } EXT;
  struct EECache   { bool have=false; String mac, serial, region; } EE;

  static uint32_t pkt_total=0;

  auto hexOrDec=[](const String& s)->int{ if(s.startsWith("0x")||s.startsWith("0X")) return (int)strtol(s.c_str(),nullptr,16); return s.toInt(); };
  auto makeString=[](const char* p,size_t n)->String{ static char buf[512]; size_t L=(n<511)?n:511; memcpy(buf,p,L); buf[L]=0; String s(buf); s.trim(); return s; };

  TypeDUDP::Packet pk;
  while (TypeDUDP::next(pk)){
    pkt_total++;
    const size_t n = pk.rx_len;
    const char* d  = pk.data;

    // -------- EE text
    if (n>=3 && d[0]=='E' && d[1]=='E' && d[2]==':'){
      String s = makeString(d,n);
      EE.have = true;
      int pos=3;
      while (pos < (int)s.length()){
        int eq = s.indexOf('=', pos); if (eq<0) break;
        String key = s.substring(pos, eq);
        int bar = s.indexOf('|', eq+1);
        String val = (bar<0) ? s.substring(eq+1) : s.substring(eq+1, bar);
        key.trim(); val.trim();
        if      (key.equalsIgnoreCase("SN") || key.equalsIgnoreCase("SER")) EE.serial = val;
        else if (key.equalsIgnoreCase("MAC"))                                 EE.mac    = val;
        else if (key.equalsIgnoreCase("REG"))                                 EE.region = val;
        pos = (bar<0) ? s.length() : (bar+1);
      }
      continue;
    }

    // -------- MAIN text
    if (n>=2 && (d[0]=='A'||d[0]=='a') && (d[1]==','||d[1]==':')){
      String s = makeString(d,n);
      MAIN.have = true;
      int p=2;
      while (p < (int)s.length()){
        int eq = s.indexOf('=', p); if (eq<0) break;
        String key = s.substring(p, eq);
        int sep = s.indexOf(',', eq+1);
        String val = (sep<0) ? s.substring(eq+1) : s.substring(eq+1, sep);
        key.trim(); val.trim();
        if      (key.equalsIgnoreCase("fan")) MAIN.fan = constrain(val.toInt(), 0, 100);
        else if (key.equalsIgnoreCase("cpu")) MAIN.cpu = val.toInt();
        else if (key.equalsIgnoreCase("amb")) MAIN.amb = val.toInt();
        else if (key.equalsIgnoreCase("app")) val.toCharArray(MAIN.app, 33);
        p = (sep<0) ? s.length() : (sep+1);
      }
      continue;
    }

    // -------- EXT text
    if (n>=2 && (d[0]=='B'||d[0]=='b') && (d[1]==','||d[1]==':')){
      String s = makeString(d,n);
      EXT.have = true;
      int p=2;
      while (p < (int)s.length()){
        int eq = s.indexOf('=', p); if (eq<0) break;
        String key = s.substring(p, eq);
        int sep = s.indexOf(',', eq+1);
        String val = (sep<0) ? s.substring(eq+1) : s.substring(eq+1, sep);
        key.trim(); val.trim();
        if      (key.equalsIgnoreCase("av"))  EXT.av  = hexOrDec(val);
        else if (key.equalsIgnoreCase("w"))   EXT.w   = val.toInt();
        else if (key.equalsIgnoreCase("h"))   EXT.h   = val.toInt();
        else if (key.equalsIgnoreCase("xb"))  EXT.xb  = val.toInt();
        else if (key.equalsIgnoreCase("enc")) EXT.enc = hexOrDec(val);
        p = (sep<0) ? s.length() : (sep+1);
      }
      continue;
    }

    // -------- MAIN binary (tolerant size & endianness)
    if (n >= 44){
      int32_t fan, cpu, amb;
      memcpy(&fan, d+0, 4); memcpy(&cpu, d+4, 4); memcpy(&amb, d+8, 4);
      if (!sane_fan(fan) && sane_fan(bswap32(fan))) fan = bswap32(fan);
      if (!sane_temp(cpu) && sane_temp(bswap32(cpu))) cpu = bswap32(cpu);
      if (!sane_temp(amb) && sane_temp(bswap32(amb))) amb = bswap32(amb);

      MAIN.have = true;
      MAIN.fan = constrain(fan, 0, 100);
      MAIN.cpu = cpu;
      MAIN.amb = amb;

      char app[33]={0};
      memcpy(app, d+12, 32); app[32]=0;
      for (int i=0;i<32;i++){ unsigned char u=(unsigned char)app[i]; if (u && (u<0x20 || u>0x7E)) app[i]=' '; }
      strncpy(MAIN.app, app, 32);
      continue;
    }

    // -------- EXT binary (tolerant size & endianness)
    if (n >= 28 && (n % 4) == 0){
      // transmitter order: tray, av, pic, xb, w, h, enc
      int32_t f[7]; memset(f,0,sizeof(f));
      memcpy(f, d, 28);

      auto fix=[&](int idx, bool (*ok)(int32_t))->int32_t{
        int32_t v=f[idx]; if (ok(v)) return v;
        int32_t s=bswap32(v); return ok(s)? s : v;
      };
      EXT.tray = fix(0, [](int32_t){return true;});
      EXT.av   = fix(1, sane_av);
      EXT.pic  = fix(2, [](int32_t){return true;});
      EXT.xb   = fix(3, sane_xb);
      EXT.w    = fix(4, sane_resw);
      EXT.h    = fix(5, sane_resh);
      EXT.enc  = fix(6, sane_enc);

      EXT.have = true;

      st_.av_raw       = EXT.av;
      st_.res_w        = EXT.w;
      st_.res_h        = EXT.h;
      st_.enc_raw      = EXT.enc;
      st_.xboxver_code = EXT.xb;

      static char av_s[28], res_s[48], enc_s[16], xb_s[24];

      snprintf(av_s, sizeof(av_s), "%s", avLabelFromRaw(EXT.av));
      st_.av_mode = av_s;

      if (EXT.w>0 && EXT.h>0){
        String mode = modeFromRes(EXT.w, EXT.h, EXT.av);
        if (mode.length() && (mode.startsWith("480") || mode.startsWith("576")))
          mode += " " + sdSystemFromH(EXT.h);
        if (mode.length()) snprintf(res_s,sizeof(res_s), "%dx%d (%s)", EXT.w, EXT.h, mode.c_str());
        else               snprintf(res_s,sizeof(res_s), "%dx%d", EXT.w, EXT.h);
        st_.resolution = res_s;
      } else {
        st_.resolution = nullptr;
      }

      snprintf(enc_s, sizeof(enc_s), "%s", encLabelFromRaw(EXT.enc));
      st_.encoder = enc_s;

      if (sane_xb(EXT.xb)){
        snprintf(xb_s, sizeof(xb_s), "%s", xboxVerFromCode(EXT.xb));
      } else if (st_.serial && *st_.serial && EXT.enc >= 0){
        String guess = guessFromSerialAndEncoder(EXT.enc, String(st_.serial));
        snprintf(xb_s, sizeof(xb_s), "%s", guess.c_str());
      } else {
        int ecode=(EXT.enc & 0xFF);
        if      (ecode==0x70) snprintf(xb_s, sizeof(xb_s), "v1.6");
        else if (ecode==0x6A) snprintf(xb_s, sizeof(xb_s), "v1.4");
        else if (ecode==0x45) snprintf(xb_s, sizeof(xb_s), "v1.0-1.3");
        else                  snprintf(xb_s, sizeof(xb_s), "Not reported");
      }
      st_.xbox_ver = xb_s;

      continue;
    }
  }

  // ---- merge MAIN
  if (MAIN.have){
    static String s_title_store;
    if (MAIN.app[0]) {
      s_title_store = MAIN.app;
      st_.title = s_title_store.c_str();
    } else if (!st_.title || !*st_.title) {
      st_.title = "Type-D";
    }

    st_.cpu_temp_c  = MAIN.cpu;
    st_.amb_temp_c  = MAIN.amb;
    st_.fan_percent = (MAIN.fan==INT32_MIN) ? -1 : MAIN.fan;
  }

  // ---- merge EXT (in case only text path filled it)
  if (EXT.have){
    st_.av_raw       = EXT.av;
    st_.res_w        = EXT.w;
    st_.res_h        = EXT.h;
    st_.enc_raw      = EXT.enc;
    st_.xboxver_code = EXT.xb;

    static char av_s2[28], res_s2[48], enc_s2[16], xb_s2[24];
    snprintf(av_s2, sizeof(av_s2), "%s", avLabelFromRaw(EXT.av));
    st_.av_mode = av_s2;

    if (EXT.w>0 && EXT.h>0){
      String mode = modeFromRes(EXT.w, EXT.h, EXT.av);
      if (mode.length() && (mode.startsWith("480")||mode.startsWith("576")))
        mode += " " + sdSystemFromH(EXT.h);
      if (mode.length()) snprintf(res_s2,sizeof(res_s2), "%dx%d (%s)", EXT.w, EXT.h, mode.c_str());
      else               snprintf(res_s2,sizeof(res_s2), "%dx%d", EXT.w, EXT.h);
      st_.resolution = res_s2;
    }

    snprintf(enc_s2, sizeof(enc_s2), "%s", encLabelFromRaw(EXT.enc));
    st_.encoder = enc_s2;

    if (sane_xb(EXT.xb)){
      snprintf(xb_s2, sizeof(xb_s2), "%s", xboxVerFromCode(EXT.xb));
    } else if (st_.serial && *st_.serial && EXT.enc >= 0){
      String guess = guessFromSerialAndEncoder(EXT.enc, String(st_.serial));
      snprintf(xb_s2, sizeof(xb_s2), "%s", guess.c_str());
    } else {
      int ecode=(EXT.enc & 0xFF);
      if      (ecode==0x70) snprintf(xb_s2, sizeof(xb_s2), "v1.6");
      else if (ecode==0x6A) snprintf(xb_s2, sizeof(xb_s2), "v1.4");
      else if (ecode==0x45) snprintf(xb_s2, sizeof(xb_s2), "v1.0-1.3");
      else                  snprintf(xb_s2, sizeof(xb_s2), "Not reported");
    }
    st_.xbox_ver = xb_s2;
  }

  // ---- merge EE
  if (EE.have){
    static String s_mac, s_serial, s_region;
    if (EE.mac.length())    { s_mac    = EE.mac;    st_.mac    = s_mac.c_str(); }
    if (EE.serial.length()) { s_serial = EE.serial; st_.serial = s_serial.c_str(); }
    if (EE.region.length()) { s_region = EE.region; st_.region = s_region.c_str(); }

    if ((!st_.xbox_ver || !*st_.xbox_ver) && st_.enc_raw>=0 && s_serial.length()){
      static char xb_guess[24];
      String guess = guessFromSerialAndEncoder(st_.enc_raw, s_serial);
      snprintf(xb_guess, sizeof(xb_guess), "%s", guess.c_str());
      st_.xbox_ver = xb_guess;
    }
  }

  st_.pkt_count = pkt_total;
}

// ================= formatting + pages =================
void US2066View::padTrim(char* dst,const char* src,uint8_t width,bool center){
  auto safech=[](char c)->char{ unsigned char u=(unsigned char)c; return (u>=0x20 && u<=0x7E)? c : ' '; };
  if (!src) src="";
  size_t srclen=strlen(src), n=(srclen>width)? width: srclen;
  memset(dst, ' ', width);
  if (center){
    int pad=(int)width-(int)n, left=pad>0? pad/2:0;
    for (size_t i=0;i<n;++i) dst[left+i]=safech(src[i]);
  } else {
    for (size_t i=0;i<n;++i) dst[i]=safech(src[i]);
  }
  dst[width]='\0';
}

void US2066View::fmtUptime(char* out,size_t cap,uint32_t ms){
  uint32_t s=ms/1000, m=s/60; s%=60; uint32_t h=m/60; m%=60;
  if (h) snprintf(out, cap, "Up:%u:%02u:%02u", h, m, s);
  else   snprintf(out, cap, "Up:%u:%02u", m, s);
}

void US2066View::fmtTempsFan(char* out,size_t cap,int cpuC,int ambC,int fanPct){
  char cbuf[8]="--", abuf[8]="--", fbuf[8]="--";
  if (cpuC != INT32_MIN) snprintf(cbuf,sizeof(cbuf),"%d",cpuC);
  if (ambC != INT32_MIN) snprintf(abuf,sizeof(abuf),"%d",ambC);
  if (fanPct >= 0)       snprintf(fbuf,sizeof(fbuf),"%d%%",fanPct);
  char line[32]; snprintf(line,sizeof(line),"C:%s A:%s F:%s", cbuf, abuf, fbuf);
  padTrim(out, line, 20, true);
}

void US2066View::fitMac(char* out,size_t,const char* mac){
  if (!mac || !*mac){ padTrim(out, "MAC:N/A", 20, true); return; }
  char labeled[32]; snprintf(labeled,sizeof(labeled),"MAC:%s", mac);
  if (strlen(labeled) <= 20) padTrim(out, labeled, 20, true);
  else if (strlen(mac) <= 20) padTrim(out, mac, 20, true);
  else { size_t n=strlen(mac); const char* p=mac + (n>17 ? n-17 : 0); padTrim(out, p, 20, true); }
}

void US2066View::centerOrLeft(char* out,size_t cap,const char* src,bool center){
  padTrim(out, src ? src : "N/A", 20, center);
}

// Page 0 — App title (marquee) + temps + AV + res
void US2066View::drawPageA(){
  char L0[21],L1[21],L2[21],L3[21];

  // --- App title marquee (20-char window) ---
  const char* t = st_.title ? st_.title : "Type-D";
  static String last_title;
  static String marquee_buf;
  static uint32_t last_ms = 0;
  static int offset = 0;
  const uint32_t step_ms = 350;

  if (!last_title.equals(String(t))) {
    last_title = String(t);
    marquee_buf = last_title + "   " + last_title + "   ";
    offset = 0;
    last_ms = 0;
  }

  if ((int)last_title.length() <= 20) {
    centerOrLeft(L0, sizeof(L0), t, true);
  } else {
    uint32_t now = millis();
    if (now - last_ms >= step_ms) { last_ms = now; offset = (offset + 1) % marquee_buf.length(); }
    char vis[21];
    for (int i=0;i<20;++i) vis[i] = marquee_buf[(offset + i) % marquee_buf.length()];
    vis[20] = 0;
    padTrim(L0, vis, 20, true);
  }

  fmtTempsFan(L1,sizeof(L1), st_.cpu_temp_c, st_.amb_temp_c, st_.fan_percent);

  const char* av_txt = st_.av_mode;
  char av_buf[28];
  if ((!av_txt || !*av_txt) && st_.av_raw>=0){ snprintf(av_buf,sizeof(av_buf),"%s", avLabelFromRaw(st_.av_raw)); av_txt=av_buf; }
  char r2[40]; snprintf(r2,sizeof(r2), "AV:%s", (av_txt&&*av_txt)? av_txt : "N/A");
  padTrim(L2, r2, 20, true);

  const char* res_txt = st_.resolution;
  char res_buf[48];
  if ((!res_txt || !*res_txt) && (st_.res_w>0 && st_.res_h>0)){
    String mode = modeFromRes(st_.res_w, st_.res_h, st_.av_raw);
    if (mode.length() && (mode.startsWith("480") || mode.startsWith("576")))
      mode += " " + sdSystemFromH(st_.res_h);
    if (mode.length()) snprintf(res_buf,sizeof(res_buf), "%dx%d (%s)", st_.res_w, st_.res_h, mode.c_str());
    else               snprintf(res_buf,sizeof(res_buf), "%dx%d", st_.res_w, st_.res_h);
    res_txt = res_buf;
  }
  char r3[64]; snprintf(r3,sizeof(r3), "Res:%s", (res_txt&&*res_txt)? res_txt : "N/A");
  const char* p=r3; size_t len=strlen(r3); if (len>20) p=r3+(len-20);
  padTrim(L3, p, 20, true);

  d_->writeLine(0,L0,true);
  d_->writeLine(1,L1,true);
  d_->writeLine(2,L2,true);
  d_->writeLine(3,L3,true);
}

// Page 1 — Encoder/Region/MAC/Serial/Xbox ver
void US2066View::drawPageB(){
  char L0[21],L1[21],L2[21],L3[21];

  const char* enc_txt = st_.encoder;
  char enc_buf[16];
  if ((!enc_txt || !*enc_txt) && st_.enc_raw>=0){ snprintf(enc_buf,sizeof(enc_buf),"%s", encLabelFromRaw(st_.enc_raw)); enc_txt=enc_buf; }
  const char* reg = st_.region ? st_.region : "N/A";
  char r0[40]; snprintf(r0,sizeof(r0),"Enc:%s Reg:%s", enc_txt?enc_txt:"N/A", reg);
  if (strlen(r0)>20) snprintf(r0,sizeof(r0),"%s %s", enc_txt?enc_txt:"N/A", reg);
  padTrim(L0, r0, 20, true);

  fitMac(L1, sizeof(L1), st_.mac);

  char r2[28];
  if (st_.serial && *st_.serial) snprintf(r2,sizeof(r2),"SN:%s", st_.serial);
  else                           snprintf(r2,sizeof(r2),"SN:N/A");
  padTrim(L2, r2, 20, true);

  const char* xb_txt = st_.xbox_ver;
  char r3[28]; snprintf(r3,sizeof(r3),"XBOX Ver:%s", (xb_txt&&*xb_txt)? xb_txt : "Not reported");
  padTrim(L3, r3, 20, true);

  d_->writeLine(0,L0,true);
  d_->writeLine(1,L1,true);
  d_->writeLine(2,L2,true);
  d_->writeLine(3,L3,true);
}

// Page 2 — WiFi/IP/Batt/Uptime
void US2066View::drawPage3(){
  char L0[21],L1[21],L2[21],L3[21];

  { char b[24]; if (st_.rssi_dbm!=INT32_MIN) snprintf(b,sizeof(b),"WiFi:%d dBm", st_.rssi_dbm); else snprintf(b,sizeof(b),"WiFi:N/A"); padTrim(L0,b,20,true); }
  { char b[28]; if (st_.ip && *st_.ip) snprintf(b,sizeof(b),"IP:%s", st_.ip); else snprintf(b,sizeof(b),"IP:N/A"); padTrim(L1,b,20,true); }

  if (st_.batt_percent>=0 || st_.batt_volts>=0.0f){
    char b[24];
    if (st_.batt_percent>=0 && st_.batt_volts>=0.0f) snprintf(b,sizeof(b),"Batt:%3d%% %4.2fV", st_.batt_percent, st_.batt_volts);
    else if (st_.batt_percent>=0)                     snprintf(b,sizeof(b),"Batt:%3d%%",        st_.batt_percent);
    else                                              snprintf(b,sizeof(b),"Batt:%4.2fV",       st_.batt_volts);
    padTrim(L2,b,20,true);
  } else {
    padTrim(L2," ",20,true);
  }

  { char up[20]; fmtUptime(up,sizeof(up),st_.uptime_ms); char b[28];
    if (st_.pkt_count>0) snprintf(b,sizeof(b),"%s Pkts:%lu", up, (unsigned long)st_.pkt_count);
    else                 snprintf(b,sizeof(b),"%s", up);
    padTrim(L3,b,20,true);
  }

  d_->writeLine(0,L0,true);
  d_->writeLine(1,L1,true);
  d_->writeLine(2,L2,true);
  d_->writeLine(3,L3,true);
}

void US2066View::drawPage4() {
  loadWeatherPrefsOnce();
  if (!W.enabled) { drawPageA(); return; }
  if (!W.ok) fetchWeatherNow();

  char L0[21], L1[21], L2[21], L3[21];

  // --------- Line 0: compact header with ellipsis (no marquee) ----------
  const char* headCore = (W.place[0] ? W.place : (W.code >= 0 ? labelForCode(W.code) : "Weather"));
  char header[64];
  snprintf(header, sizeof(header), "%s", headCore);
  size_t hlen = strlen(header);

  if (hlen <= 20) {
    padTrim(L0, header, 20, true);
  } else {
    // Keep start and end, insert "..." in the middle to fit 20 chars
    // 11 + 3 + 6 = 20
    const int PRE = 11;
    const int SUF = 6;
    char vis[21];
    memcpy(vis, header, PRE);
    vis[PRE+0] = '.';
    vis[PRE+1] = '.';
    vis[PRE+2] = '.';
    memcpy(vis + PRE + 3, header + (hlen - SUF), SUF);
    vis[20] = '\0';
    padTrim(L0, vis, 20, true);
  }

  // --------- Line 1: Temp ----------
  char l1[24];
  if (!isnan(W.temp)) snprintf(l1, sizeof(l1), "Temp: %d%c", (int)lroundf(W.temp), W.units);
  else                snprintf(l1, sizeof(l1), "Temp: —");
  padTrim(L1, l1, 20, true);

  // --------- Line 2: Humidity ----------
  if (W.rh >= 0) { char l2[24]; snprintf(l2, sizeof(l2), "Humidity: %d%%", W.rh); padTrim(L2, l2, 20, true); }
  else           { padTrim(L2, " ", 20, true); }

  // --------- Line 3: Updated age / Fetching ----------
  uint32_t now = millis();
  uint32_t age_ms = (now >= W.last_fetch) ? (now - W.last_fetch) : 0;
  uint32_t age_min = age_ms / 60000UL;
  char l3[24];
  if (W.ok) { if (age_min < 1) snprintf(l3, sizeof(l3), "Updated: <1m"); else snprintf(l3, sizeof(l3), "Updated: %lum", (unsigned long)age_min); }
  else      { snprintf(l3, sizeof(l3), "Fetching…"); }
  padTrim(L3, l3, 20, true);

  d_->writeLine(0, L0, true);
  d_->writeLine(1, L1, true);
  d_->writeLine(2, L2, true);
  d_->writeLine(3, L3, true);
}

