#pragma once
#include <Arduino.h>

// Forward decl to avoid template headaches; pass your concrete u8g2 instance pointer at begin().
class U8G2;

namespace TypeDDisplay {

// Optional: tweak holds at runtime
void setHoldTimes(uint32_t main_ms, uint32_t second_ms);

// Attach the u8g2 instance. Does NOT draw until we have UDP data.
void begin(U8G2* u8);

// Main pump: call this every loop() tick.
// It pulls new UDP packets (via TypeDUDP), updates caches, and renders screens.
void loop();

// Optional serial noise from this module (independent of UDP debug)
void setDebug(bool on);

bool active();

} // namespace TypeDDisplay
