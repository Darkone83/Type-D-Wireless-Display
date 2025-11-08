#pragma once
#include <Arduino.h>

namespace Weather {

struct Config {
  bool   enabled        = true;     // toggle weather on/off
  bool   autoLocate     = true;     // true = geolocate via IP, false = use lat/lon
  double lat            = NAN;      // used if autoLocate == false
  double lon            = NAN;      // used if autoLocate == false
  bool   useFahrenheit  = true;     // true = °F, false = °C
  String apiKey;                    // reserved (not required for Open-Meteo)
};

struct Snapshot {
  bool     ok = false;
  uint32_t ts = 0;        // millis() when refreshed
  float    tempC = NAN;   // current temperature in °C
  int      wmo = -1;      // WMO weather code
  int      humidity = -1; // %
  String   text;          // short condition text
};

void   begin();
void   loop();
void   setConfig(const Config& c);     // apply and persist
Config getConfig();
bool   isReady();                      // snapshot available
Snapshot get();                        // latest snapshot

} // namespace Weather
