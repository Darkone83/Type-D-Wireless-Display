#pragma once
#include <Arduino.h>
#include <U8g2lib.h>

namespace Insignia {

// ---- configuration / setup ----
void setServerBase(const String& base);
void begin(bool debug);

// OPTIONAL cache controls (match .cpp you just added)
void setFlushCacheOnBoot(bool enable);                    // call in setup() if you want a wipe on boot
void flushCacheNow();                                     // manual wipe
void setCacheLimits(size_t maxFiles, size_t maxBytes, uint32_t maxAgeMs);

// ---- runtime wiring ----
void onAppName(const char* app);                          // pass current app/game name
bool isActive();                                          // only true after a definite match + model loaded
void tick();                                              // call every loop (advances scroll/rotation)
void draw(U8G2* g);                                       // render when isActive() == true
uint32_t recommendedHoldMs();                             // optional scheduler hint

} // namespace Insignia
