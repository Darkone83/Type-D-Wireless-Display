#pragma once
#include <Arduino.h>
#include "us2066.h"

// Weather (lightweight surface; the module does the heavy lifting)
#include "weather.h"

struct US2066_Status {
  // Page 0 / 1
  const char* title       = nullptr;
  int   cpu_temp_c        = INT32_MIN;
  int   amb_temp_c        = INT32_MIN;
  int   fan_percent       = -1;

  // Derived or raw video info (Page 1)
  const char* av_mode     = nullptr;   // derived from av_raw if null
  const char* resolution  = nullptr;   // derived from res_w/res_h if null
  int   av_raw            = -1;
  int   res_w             = 0;
  int   res_h             = 0;

  // Encoder/xbox (Page 2)
  int   enc_raw           = -1;
  int   xboxver_code      = -1;
  const char* encoder     = nullptr;   // derived from enc_raw if null
  const char* region      = nullptr;   // from EE
  const char* mac         = nullptr;   // from EE
  const char* serial      = nullptr;   // from EE
  const char* xbox_ver    = nullptr;   // from SMC or guess

  // Page 3 (system/net)
  int   rssi_dbm          = INT32_MIN;
  const char* ip          = nullptr;
  int   batt_percent      = -1;
  float batt_volts        = -1.0f;
  uint32_t uptime_ms      = 0;
  uint32_t pkt_count      = 0;
};

class US2066View {
public:
  US2066View();

  bool attach(US2066* dev);
  void setStatus(const US2066_Status& s);

  // Backwards-compat no-ops (was used for Insignia previously)
  inline void setInsignia(...) {}
  inline void setInsigniaScrollPeriod(uint32_t) {}

  void loop();

  void setPagePeriod(uint32_t ms);
  void forcePage(uint8_t idx);
  void clear();
  void splash(const char* l0, const char* l1=nullptr,
              const char* l2=nullptr, const char* l3=nullptr);

private:
  US2066* d_ = nullptr;
  US2066_Status st_{};

  uint8_t  page_ = 0;
  uint32_t last_page_ms_ = 0;
  uint32_t page_ms_ = 4500;

  // UDP ingest + merge into status
  void drainUdpAndMergeStatus();

  // pages
  void drawPageA();  // App + temps + AV + res
  void drawPageB();  // Encoder/region/MAC/Serial/Xbox ver
  void drawPage3();  // WiFi/IP/Batt/Uptime
  void drawPage4();  // Weather (if enabled & ready); else skipped

  // helpers
  static void padTrim(char* dst, const char* src, uint8_t width, bool center=false);
  static void fmtUptime(char* out, size_t cap, uint32_t ms);
  static void fmtTempsFan(char* out, size_t cap, int cpuC, int ambC, int fanPct);
  static void fitMac(char* out, size_t cap, const char* mac);
  static void centerOrLeft(char* out, size_t cap, const char* src, bool center=true);
};
