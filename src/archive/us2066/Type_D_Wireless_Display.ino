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
static const int PIN_RST = 9;   // SSD1309 RESET wired to GPIO 9

U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0, /* reset = */ PIN_RST);

// US2066 objects
US2066      charOled;
US2066View  charView;
bool        useUS2066 = false;

// ---- helpers to init US2066 path ----
static bool initUS2066() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!charOled.begin(&Wire, 0x3C, 20, 4)) {
    return false; // fallback to SSD1309
  }
  if (!charView.attach(&charOled)) {
    return false; // fallback to SSD1309
  }
  // simple boot splash (one-time). The view owns data thereafter.
  charView.splash("Type-D Wireless", "US2066 20x4", "Alt View Ready", "");
  return true;
}

void setup() {
  //Serial.begin(115200);

  LedStat::begin();
  LedStat::setStatus(LedStatus::Booting);
  WiFiMgr::begin();

  // Decide active display from portal pref
  useUS2066 = WiFiMgr::isUS2066Selected();

  if (useUS2066) {
    if (!initUS2066()) {
      useUS2066 = false; // fallback to SSD1309 below
    }
  }

  if (!useUS2066) {
    // ---- SSD1309 path ----
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);

    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    delay(20);
    digitalWrite(PIN_RST, HIGH);
    delay(250);

    u8g2.setI2CAddress(0x3D << 1);
    u8g2.begin();
    u8g2.setContrast(255);
    u8g2.setFlipMode(1); // use 0/1 as needed

    TypeDDisplay::begin(&u8g2);
    TypeDDisplay::setHoldTimes(15000, 5000);
  }

  Weather::begin();       // drive weather for BOTH displays (SSD1309 & US2066)

  TypeDUDP::begin();      // sockets bind on Wi-Fi connect and rebind on drops

  // Insignia emulator/runtime (shared across displays)
  Insignia::setServerBase("http://darkone83.myddns.me:8080/xbox, http://darkone83.myddns.me:8008/xbox/data");
  Insignia::setFlushCacheOnBoot(true);
  Insignia::setCacheLimits(32, 128*1024, 6UL*60*60*1000UL);
  Insignia::begin(/*debug=*/true);
}

void loop() {
  LedStat::loop();
  WiFiMgr::loop();

  // Drive the weather state machine regardless of display type.
  // (US2066 and SSD1309 views both read Weather::isReady()/get().)
  Weather::loop();

  // Always pump UDP first so display sees the freshest packet this tick
  TypeDUDP::loop();

  if (useUS2066) {
    // US2066 view ingests UDP (MAIN/EXT/EE), reads IP/RSSI, formats, and renders.
    charView.loop();   // <- parses MAIN/EXT/EE and renders pages (incl. weather page)
    Insignia::tick();  // <- tick AFTER view so Insignia reacts immediately if needed
  } else {
    // SSD1309 path (existing UI)
    TypeDDisplay::loop();
    Insignia::tick();  // SSD1309 timing unchanged
  }

  static bool mdnsStarted = false;
  const bool connected = WiFiMgr::isConnected();
  if (connected && !mdnsStarted) {
    if (MDNS.begin("typeddisp")) {
      mdnsStarted = true;
    }
  }
}
