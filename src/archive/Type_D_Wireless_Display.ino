#include "wifimgr.h"
#include "led_stat.h"
#include "udp_typed.h"
#include "display.h"
#include "weather.h"

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <ESPmDNS.h>

// -------- OLED (Waveshare 2.42" SSD1309 I2C @ 0x3D) --------
static const int PIN_SDA = 6;
static const int PIN_SCL = 7;
static const int PIN_RST = U8X8_PIN_NONE;   // set to your RESET GPIO if wired

// If you see shifted output, switch to NONAME0.
U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0, /* reset = */ PIN_RST);

void setup() {
  Serial.begin(115200);

  // --- OLED bring-up on exact pins ---
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  u8g2.setI2CAddress(0x3D << 1);   // panel address 0x3D
  u8g2.begin();
  u8g2.setContrast(255);
  // u8g2.setFlipMode(1); // if upside-down

  // --- status + WiFi ---
  LedStat::begin();
  LedStat::setStatus(LedStatus::Booting);
  WiFiMgr::begin();

  // Weather service (portals + fetch cadence live elsewhere)
  Weather::begin();

  // Display module (rendering once MAIN UDP arrives; handles boot logo / idle)
  TypeDDisplay::begin(&u8g2);
  TypeDDisplay::setHoldTimes(15000, 5000);

  // UDP: arm immediately; sockets bind on Wi-Fi connect and rebind on drops
  TypeDUDP::begin();
}

void loop() {
  LedStat::loop();
  WiFiMgr::loop();
  Weather::loop();

  // Always pump UDP first so display sees the freshest packet this tick
  TypeDUDP::loop();

  // Let the display module take over once packets arrive (handles transitions)
  TypeDDisplay::loop();

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
