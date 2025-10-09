#include "wifimgr.h"
#include "led_stat.h"
#include "udp_typed.h"
#include "display.h"
#include "weather.h"
#include "insignia.h"

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <ESPmDNS.h>

// -------- Character OLED (US2066 20x4) --------
#include "us2066.h"
#include "us2066_view.h"

// -------- OLED (Waveshare 2.42" SSD1309 I2C @ 0x3D) --------
static const int PIN_SDA = 6;
static const int PIN_SCL = 7;
static const int PIN_RST = 8;   // SSD1309 RESET is wired to GPIO 8

// If you see shifted output, switch to NONAME0.
U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0, /* reset = */ PIN_RST);

// US2066 objects
US2066      charOled;
US2066View  charView;
bool        useUS2066 = false;

// ---- helpers to init US2066 path ----
static bool initUS2066() {
  // Same I2C pins as SSD1309; address typically 0x3C on US2066 modules
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!charOled.begin(&Wire, 0x3C, 20, 4)) {
    Serial.println("[US2066] not detected on 0x3C, fallback to SSD1309");
    return false;
  }
  if (!charView.attach(&charOled)) {
    Serial.println("[US2066] attach failed, fallback to SSD1309");
    return false;
  }
  charView.splash("Type-D Wireless", "US2066 20x4", "Alt View Ready", "");
  return true;
}

// ---- optional stubs (replace with real data sources when ready) ----
static const char* currentTitle()     { return "Type-D"; } // TODO: wire from your UDP/display state
static const char* currentAvMode()    { return nullptr; }  // "HDMI","YPbPr","VGA","COMP"
static const char* currentRes()       { return nullptr; }  // "1280x720@60"
static const char* currentEncoder()   { return nullptr; }  // "FOCUS","CONEX","XCAL"
static const char* currentRegion()    { return nullptr; }  // "NTSC","PAL","NTSC-J"
static const char* macString()        { return WiFi.macAddress().c_str(); }
static const char* xboxSerial()       { return nullptr; }  // your source if available
static const char* xboxVersion()      { return nullptr; }  // e.g., "1.6b"
static int         readCpuC()         { return INT32_MIN; } // unknown -> gracefully shown as "C:--"
static int         readAmbC()         { return INT32_MIN; }
static int         readFanPct()       { return -1; }
static int         currentRSSI()      { return (WiFi.status()==WL_CONNECTED) ? WiFi.RSSI() : INT32_MIN; }
static uint32_t    packetTotal()      { return 0; } // optional: sum from your UDP counters

void setup() {
  Serial.begin(115200);

  // Status LED + Wi-Fi manager (loads display preference)
  LedStat::begin();
  LedStat::setStatus(LedStatus::Booting);
  WiFiMgr::begin();

  // Decide active display from portal pref
  useUS2066 = WiFiMgr::isUS2066Selected();

  if (useUS2066) {
    // ---- US2066 path (skip SSD1309 init) ----
    if (!initUS2066()) {
      useUS2066 = false; // fallback to SSD1309 below
    }
  }

  if (!useUS2066) {
    // ---- SSD1309 path (original bring-up) ----
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);

    // deterministic hardware reset for SSD1309
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    delay(20);
    digitalWrite(PIN_RST, HIGH);
    delay(250);

    u8g2.setI2CAddress(0x3D << 1); // panel address 0x3D
    u8g2.begin();
    u8g2.setContrast(255);
    // u8g2.setFlipMode(1); // if upside-down

    // Display module (uses u8g2)
    TypeDDisplay::begin(&u8g2);
    TypeDDisplay::setHoldTimes(15000, 5000);
  }

  // Weather service: init always; we'll skip loop() when US2066 is active
  Weather::begin();

  // UDP: sockets bind on Wi-Fi connect and rebind on drops
  TypeDUDP::begin();

  // Insignia emulator/runtime (shared across displays)
  Insignia::setFlushCacheOnBoot(true);                  // optional: wipe cache once at boot
  Insignia::setCacheLimits(32, 128*1024, 6UL*60*60*1000UL);
  Insignia::begin(/*debug=*/true);
}

void loop() {
  LedStat::loop();
  WiFiMgr::loop();

  // Only run full weather pipeline for SSD1309 UI
  if (!useUS2066) {
    Weather::loop();
  }

  // Always pump UDP first so display sees the freshest packet this tick
  TypeDUDP::loop();

  if (useUS2066) {
    // ---- Character OLED pages (no weather) ----
    US2066_Status s{};
    s.title        = currentTitle();
    s.cpu_temp_c   = readCpuC();
    s.amb_temp_c   = readAmbC();
    s.fan_percent  = readFanPct();
    s.av_mode      = currentAvMode();
    s.resolution   = currentRes();

    s.encoder      = currentEncoder();
    s.region       = currentRegion();
    s.mac          = macString();
    s.serial       = xboxSerial();
    s.xbox_ver     = xboxVersion();

    s.rssi_dbm     = currentRSSI();
    s.ip           = WiFi.isConnected() ? WiFi.localIP().toString().c_str() : nullptr;
    s.batt_percent = -1;
    s.batt_volts   = -1.0f;
    s.uptime_ms    = millis();
    s.pkt_count    = packetTotal();

    charView.setStatus(s);

    // Optional: provide Insignia lines when you have them.
    // For now, leave empty; the Insignia page will show the title & blank rows.
    // US2066_InsigniaFeed ins{};
    // ins.title = "Insignia";
    // ins.lines = {/* e.g., "1. GT  12,345", "2. ..." */};
    // charView.setInsignia(ins);

    charView.loop();

  } else {
    // ---- SSD1309 path (your existing UI) ----
    TypeDDisplay::loop();
  }

  // mDNS (starts once Wi-Fi is up)
  static bool mdnsStarted = false;
  const bool connected = WiFiMgr::isConnected();
  if (connected && !mdnsStarted) {
    if (MDNS.begin("typeddisp")) {
      Serial.println("[mDNS] Started: http://typeddisp.local/");
      mdnsStarted = true;
    } else {
      Serial.println("[mDNS] mDNS start failed");
    }
  }
}
