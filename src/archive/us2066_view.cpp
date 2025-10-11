#include "us2066_view.h"
#include <string.h>
#include <stdio.h>

US2066View::US2066View() {}

bool US2066View::attach(US2066* dev) {
  d_ = dev;
  if (!d_) return false;
  if (!d_->ping()) return false;
  d_->clear();
  d_->displayOn(true);
  return true;
}

void US2066View::setStatus(const US2066_Status& s) { st_ = s; }
void US2066View::setInsignia(const US2066_InsigniaFeed& f) {
  insig_.title = f.title ? f.title : "Insignia";
  insig_.lines = f.lines; // copy
  // reset scroll when feed updates
  insig_offset_ = 0;
  insig_last_ms_ = 0;
}

void US2066View::setPagePeriod(uint32_t ms) { if (ms) page_ms_ = ms; }
void US2066View::setInsigniaScrollPeriod(uint32_t ms) { if (ms) insig_step_ms_ = ms; }

void US2066View::forcePage(uint8_t idx) {
  page_ = idx % 4;
  last_page_ms_ = millis();
}

void US2066View::clear() {
  if (d_) d_->clear();
}

void US2066View::splash(const char* l0, const char* l1, const char* l2, const char* l3) {
  if (!d_) return;
  d_->clear();
  d_->writeLine(0, (l0? l0:""), true);
  d_->writeLine(1, (l1? l1:""), true);
  d_->writeLine(2, (l2? l2:""), true);
  d_->writeLine(3, (l3? l3:""), true);
}

void US2066View::loop() {
  if (!d_) return;

  uint32_t now = millis();
  if (now - last_page_ms_ >= page_ms_) {
    page_ = (page_ + 1) % 4;
    last_page_ms_ = now;
  }

  switch (page_) {
    case 0: drawPageA(); break;
    case 1: drawPageB(); break;
    case 2: drawPage3(); break;
    default: drawPage4(); break;
  }
}

// ---------------- helpers ----------------
void US2066View::padTrim(char* dst, const char* src, uint8_t width, bool center) {
  if (!src) src = "";
  size_t n = strlen(src); if (n > width) n = width;
  if (center) {
    int pad = (int)width - (int)n;
    int left = pad > 0 ? pad/2 : 0;
    memset(dst, ' ', width);
    memcpy(dst + left, src, n);
  } else {
    memset(dst, ' ', width);
    memcpy(dst, src, n);
  }
  dst[width] = '\0';
}

void US2066View::fmtUptime(char* out, size_t cap, uint32_t ms) {
  uint32_t s = ms / 1000;
  uint32_t m = s / 60; s %= 60;
  uint32_t h = m / 60; m %= 60;
  if (h) snprintf(out, cap, "Up:%u:%02u:%02u", h, m, s);
  else   snprintf(out, cap, "Up:%u:%02u", m, s);
}

void US2066View::fmtTempsFan(char* out, size_t cap, int cpuC, int ambC, int fanPct) {
  // C=<cpu>  A=<amb>  F=<fan%>  — compact, fits 20 chars centered
  char cbuf[8]="--", abuf[8]="--", fbuf[8]="--";
  if (cpuC != INT32_MIN) snprintf(cbuf, sizeof(cbuf), "%d", cpuC);
  if (ambC != INT32_MIN) snprintf(abuf, sizeof(abuf), "%d", ambC);
  if (fanPct >= 0)       snprintf(fbuf, sizeof(fbuf), "%d%%", fanPct);
  char line[32];
  snprintf(line, sizeof(line), "C:%s A:%s F:%s", cbuf, abuf, fbuf);
  padTrim(out, line, 20, true);
}

void US2066View::fitMac(char* out, size_t cap, const char* mac) {
  if (!mac || !*mac) { padTrim(out, "MAC:—", 20, true); return; }
  // If "MAC: xx:xx:xx:xx:xx:xx" (22) doesn't fit, prefer bare address (17)
  char labeled[32]; snprintf(labeled, sizeof(labeled), "MAC:%s", mac);
  if (strlen(labeled) <= 20) {
    padTrim(out, labeled, 20, true);
  } else if (strlen(mac) <= 20) {
    padTrim(out, mac, 20, true);
  } else {
    // fallback: last 17 chars (usually full addr)
    size_t n = strlen(mac);
    const char* p = mac + (n > 17 ? (n - 17) : 0);
    padTrim(out, p, 20, true);
  }
}

void US2066View::centerOrLeft(char* out, size_t cap, const char* src, bool center) {
  padTrim(out, src ? src : "—", 20, center);
}

// ---------------- pages ----------------

// PAGE A: Title / Temps+Fan / AV / Resolution
void US2066View::drawPageA() {
  char L0[21], L1[21], L2[21], L3[21];
  // L0: title centered
  centerOrLeft(L0, sizeof(L0), st_.title ? st_.title : "Type-D", true);
  // L1: temps + fan (centered)
  fmtTempsFan(L1, sizeof(L1), st_.cpu_temp_c, st_.amb_temp_c, st_.fan_percent);
  // L2: AV mode centered
  {
    char buf[24]; snprintf(buf, sizeof(buf), "AV:%s", st_.av_mode ? st_.av_mode : "—");
    padTrim(L2, buf, 20, true);
  }
  // L3: Resolution centered
  {
    char buf[24]; snprintf(buf, sizeof(buf), "Res:%s", st_.resolution ? st_.resolution : "—");
    padTrim(L3, buf, 20, true);
  }

  d_->writeLine(0, L0, true);
  d_->writeLine(1, L1, true);
  d_->writeLine(2, L2, true);
  d_->writeLine(3, L3, true);
}

// PAGE B: Encoder+Region / MAC / Serial / Xbox version
void US2066View::drawPageB() {
  char L0[21], L1[21], L2[21], L3[21];

  // L0: "Enc:FOCUS Reg:NTSC" (fits 20)
  {
    const char* enc = st_.encoder ? st_.encoder : "—";
    const char* reg = st_.region  ? st_.region  : "—";
    char buf[32]; snprintf(buf, sizeof(buf), "Enc:%s Reg:%s", enc, reg);
    padTrim(L0, buf, 20, true);
  }
  // L1: MAC (auto-fit)
  fitMac(L1, sizeof(L1), st_.mac);

  // L2: Serial (center)
  {
    char buf[28];
    if (st_.serial && *st_.serial) snprintf(buf, sizeof(buf), "SN:%s", st_.serial);
    else snprintf(buf, sizeof(buf), "SN:—");
    padTrim(L2, buf, 20, true);
  }

  // L3: Xbox version (center)
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "Ver:%s", (st_.xbox_ver && *st_.xbox_ver) ? st_.xbox_ver : "—");
    padTrim(L3, buf, 20, true);
  }

  d_->writeLine(0, L0, true);
  d_->writeLine(1, L1, true);
  d_->writeLine(2, L2, true);
  d_->writeLine(3, L3, true);
}

// PAGE 3: Health/Net (no Region)
void US2066View::drawPage3() {
  char L0[21], L1[21], L2[21], L3[21];

  // L0: WiFi RSSI (center)
  {
    char buf[24];
    if (st_.rssi_dbm != INT32_MIN) snprintf(buf, sizeof(buf), "WiFi:%d dBm", st_.rssi_dbm);
    else                           snprintf(buf, sizeof(buf), "WiFi:—");
    padTrim(L0, buf, 20, true);
  }

  // L1: IP (center)
  {
    char buf[28];
    if (st_.ip && *st_.ip) snprintf(buf, sizeof(buf), "IP:%s", st_.ip);
    else                   snprintf(buf, sizeof(buf), "IP:—");
    padTrim(L1, buf, 20, true);
  }

  // L2: Battery if available (center)
  if (st_.batt_percent >= 0 || st_.batt_volts >= 0.0f) {
    char buf[24];
    if (st_.batt_percent >= 0 && st_.batt_volts >= 0.0f) {
      snprintf(buf, sizeof(buf), "Batt:%3d%% %4.2fV", st_.batt_percent, st_.batt_volts);
    } else if (st_.batt_percent >= 0) {
      snprintf(buf, sizeof(buf), "Batt:%3d%%", st_.batt_percent);
    } else {
      snprintf(buf, sizeof(buf), "Batt:%4.2fV", st_.batt_volts);
    }
    padTrim(L2, buf, 20, true);
  } else {
    padTrim(L2, " ", 20, true);
  }

  // L3: Uptime + Packets (center)
  {
    char up[20]; fmtUptime(up, sizeof(up), st_.uptime_ms);
    char buf[28];
    if (st_.pkt_count > 0) snprintf(buf, sizeof(buf), "%s Pkts:%lu", up, (unsigned long)st_.pkt_count);
    else                   snprintf(buf, sizeof(buf), "%s", up);
    padTrim(L3, buf, 20, true);
  }

  d_->writeLine(0, L0, true);
  d_->writeLine(1, L1, true);
  d_->writeLine(2, L2, true);
  d_->writeLine(3, L3, true);
}

// PAGE 4: Insignia simplified (title centered + 3-line scrolling list)
void US2066View::drawPage4() {
  char L0[21], L1[21], L2[21], L3[21];
  padTrim(L0, insig_.title ? insig_.title : "Insignia", 20, true);

  // Advance scroll window every insig_step_ms_
  uint32_t now = millis();
  if (now - insig_last_ms_ >= insig_step_ms_) {
    insig_last_ms_ = now;
    if (!insig_.lines.empty()) {
      insig_offset_++;
      if (insig_offset_ > (int)insig_.lines.size()) insig_offset_ = 0; // wrap with one blank pass
    }
  }

  auto getLine = [&](int idx)->const char* {
    if (idx < 0 || idx >= (int)insig_.lines.size()) return "";
    return insig_.lines[(size_t)idx].c_str();
  };

  // Window: top at insig_offset_, then next two lines
  const char* s1 = getLine(insig_offset_);
  const char* s2 = getLine(insig_offset_ + 1);
  const char* s3 = getLine(insig_offset_ + 2);

  // Center leaderboard lines for clean look
  centerOrLeft(L1, sizeof(L1), s1, true);
  centerOrLeft(L2, sizeof(L2), s2, true);
  centerOrLeft(L3, sizeof(L3), s3, true);

  d_->writeLine(0, L0, true);
  d_->writeLine(1, L1, true);
  d_->writeLine(2, L2, true);
  d_->writeLine(3, L3, true);
}
