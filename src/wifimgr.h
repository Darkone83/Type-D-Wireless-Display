#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace WiFiMgr {

    AsyncWebServer& getServer(); // keep commented if you don't expose it

    void begin();
    void loop();
    void restartPortal();
    void forgetWiFi();
    bool isConnected();
    String getStatus();

    // NEW: persisted display selection
    // Returns "ssd1309" (default) or "us2066"
    String getDisplay();

    // Convenience: true when the US2066 20x4 display is selected
    bool   isUS2066Selected();
}
