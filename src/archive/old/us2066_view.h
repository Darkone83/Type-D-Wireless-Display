#pragma once
#include <Arduino.h>
#include "us2066.h"
#include <vector>

// US2066 20x4 "alternate display" with 4 pages (no weather).
// PAGE A: Title (ctr), Temps/Fan, AV mode, Resolution
// PAGE B: Encoder+Region, MAC, Serial, Xbox version
// PAGE 3: Health/Net (RSSI, IP, Battery, Uptime/Pkts)  [Region moved to Page B]
// PAGE 4: Simplified Insignia (title + 3-line scrolling leaderboard)

struct US2066_Status {
  // Page A
  const char* title       = nullptr;   // app/game or "Type-D"
  int   cpu_temp_c        = INT32_MIN; // INT32_MIN => unknown
  int   amb_temp_c        = INT32_MIN; // "
  int   fan_percent       = -1;        // 0..100; -1 => unknown
  const char* av_mode     = nullptr;   // "HDMI","YPbPr","VGA","COMP" etc.
  const char* resolution  = nullptr;   // "1280x720@60" etc.

  // Page B
  const char* encoder     = nullptr;   // "CONEX","FOCUS","XCAL"
  const char* region      = nullptr;   // "NTSC","PAL","NTSC-J"
  const char* mac         = nullptr;   // "xx:xx:xx:xx:xx:xx"
  const char* serial      = nullptr;   // console serial
  const char* xbox_ver    = nullptr;   // "1.6b" etc.

  // Page 3 (health/net)
  int   rssi_dbm          = INT32_MIN; // WiFi RSSI dBm
  const char* ip          = nullptr;   // "192.168.1.23"
  int   batt_percent      = -1;        // 0..100; -1 => unknown
  float batt_volts        = -1.0f;     // <0 => unknown
  uint32_t uptime_ms      = 0;         // uptime
  uint32_t pkt_count      = 0;         // optional total packet count
};

// Optional Insignia feed for Page 4
struct US2066_InsigniaFeed {
  const char* title = "Insignia";
  std::vector<String> lines;           // preformatted leaderboard rows
};

class US2066View {
public:
  US2066View();

  bool attach(US2066* dev);                       // device must be begun already
  void setStatus(const US2066_Status& s);         // copy snapshot
  void setInsignia(const US2066_InsigniaFeed& f); // copy lines/title (page 4)
  void loop();                                    // draw current page (auto-rotates)

  // Options
  void setPagePeriod(uint32_t ms);                // default ~4500ms
  void setInsigniaScrollPeriod(uint32_t ms);      // default 800ms
  void forcePage(uint8_t idx);                    // 0..3
  void clear();
  void splash(const char* l0, const char* l1=nullptr,
              const char* l2=nullptr, const char* l3=nullptr);

private:
  US2066* d_ = nullptr;
  US2066_Status st_;
  US2066_InsigniaFeed insig_;

  uint8_t  page_ = 0;
  uint32_t last_page_ms_ = 0;
  uint32_t page_ms_ = 4500;

  // Insignia page scroll state
  uint32_t insig_last_ms_ = 0;
  uint32_t insig_step_ms_ = 800;
  int      insig_offset_  = 0; // top line index inside insig_.lines

  // page renderers
  void drawPageA();
  void drawPageB();
  void drawPage3();
  void drawPage4();

  // helpers
  static void padTrim(char* dst, const char* src, uint8_t width, bool center=false);
  static void fmtUptime(char* out, size_t cap, uint32_t ms);
  static void fmtTempsFan(char* out, size_t cap, int cpuC, int ambC, int fanPct);
  static void fitMac(char* out, size_t cap, const char* mac);
  static void centerOrLeft(char* out, size_t cap, const char* src, bool center=true);
};
