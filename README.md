# Type-D Wireless Display

A tiny OLED “status screen” for original Xbox setups (and other sources), driven over Wi‑Fi/UDP.  
Shows a main system summary, a dense second page, a health page, and an optional weather page, and insignia leaderboard page.
Designed for the Waveshare 2.42″ SSD1309 I²C panel (128×64), with added support expermental for US2066 OLED displays.
A Type D Expansion unit is required for these displays to properly work and display data.

---

## ✨ Features

- **Four screens** that auto-rotate:
  - **MAIN** – App name, fan %, CPU °C, ambient °C. Battery widget appears if a LC709203F fuel gauge is found.
  - **SECOND** – Tray state, AV type, encoder chip, guessed Xbox version, serial, MAC, region.
  - **HEALTH** – Wi‑Fi RSSI (with quality label), free heap, IP address.
  - **WEATHER** *(optional)* – Location header, big temp, condition text, humidity & wind.
  - **INSIGNIA** - Will display insignia leaderboard info when a compatible game is selected.
- **Inactivity policy (no “STALE”)**
  - Before any UDP data: full-screen boot logo only.
  - No UDP for **2 min**: show boot logo.
  - No UDP for **5 min**: bouncing “Sleeping…” screensaver.
  - Any UDP packet instantly cancels saver and resumes the carousel.
- **Smooth transitions** – slide-in left/right between screens.
- **Quote ticker** on MAIN with periodic rotation.
- **Web setup portal** (captive portal) for Wi‑Fi + Weather settings.
- **mDNS**: `http://typeddisp.local/` (when joined to your LAN).
- **UDP ring buffer** – robust ingest across three “typed” UDP ports (A/B/C).

---

## 🧰 Required Hardware

### Desktop build
- **ESP32‑S3**: Waveshare ESP32-S3 Zero or compatable.
- **Waveshare 2.42″ OLED**.
- *(Optional)* **LC709203F** fuel gauge (Adafruit breakout works great).
- *(Optional)* LiPo pack if you want the battery widget to show %/voltage.

### Controller port build
- **ESP32‑S3**: Waveshare ESP32-S3 Zero or compatable.
- **Waveshare 2.42″ OLED**.
- **2 Caps**: 1x 100uF, 1X 10uF
- **Sacrificial OG XBOX dongle**: Needed to actually interface with the xbox controller port for power.



### Wiring (I²C bus shared by OLED + LC709203F)

| Signal | ESP32‑S3 Pin | OLED | LC709203F |
|-------:|:-------------|:-----|:----------|
| SDA    | **GPIO 6**   | SDA  | SDA       |
| SCL    | **GPIO 7**   | SCL  | SCL       |
| RST    | **GPIO 9**   | RST  |           |
| 3V3    | 3V3          | VCC  | VIN       |
| GND    | GND          | GND  | GND       |

> 🔧 Pin definitions live in your `setup()` (e.g. `main.cpp`):
> ```cpp
> static const int PIN_SDA = 6;   // ESP32‑S3 default in this project
> static const int PIN_SCL = 7;
> ```
> If you’re **not** using an ESP32‑S3, move I²C to safe pins for your board (classic ESP32: avoid GPIO 6–11).

---

## 🔌 Firmware Overview

### Display / UI (`display.cpp`)
- Uses **U8g2** with the SSD1309 (128×64, I²C 0x3D).
- Screen set: **WAITING → MAIN → SECOND → HEALTH → WEATHER? → MAIN** (weather only when enabled in prefs).
- Inactivity timers (based on “any UDP packet” last‑seen).
- Battery widget appears right‑justified on the “Fan” row if LC709203F is detected.
- Weather uses **Open‑Meteo** (no key) over HTTPS (certificate check disabled for simplicity).

### UDP ingest (`udp_typed.cpp/.h`)
- Three typed sockets (“A/B/C”) with a small ring buffer.
- Auto (re)binds when Wi‑Fi connects; closes & re‑arms if Wi‑Fi drops.
- Public helpers:
  - `TypeDUDP::begin()/loop()/next()` – non‑blocking packet pump.
  - `TypeDUDP::lastSeenMs()` and queue APIs (`available()/next()`).
- Packets expected by the display:
  - **Port A** (44 bytes): `int32 fan, cpu, amb; char app[32];`
  - **Port B** (28 bytes): `int32 t, a, pic, xb, enc, x6, x7;`  
    *(code auto‑detects which two fields are width/height based on the encoder field)*
  - **Port C** (text): `EE:SN=...|MAC=...|REG=...`

> Defaults for the ports are defined in `udp_typed.h` as `UDP_TYPED_DEFAULT_PORT_A/B/C`.  
> Keep your sender in sync with those values or pass custom ports to `TypeDUDP::begin()`.

### Wi‑Fi manager / Web portal (`wifimgr.cpp`)
- Starts as **AP+STA**; launches a captive portal at **Type D Wireless Display Setup**.
- Endpoints:
  - `/` – HTML portal (Wi‑Fi scan/connect + Weather config).
  - `/scan`, `/save`, `/forget`, `/status` – Wi‑Fi management.
  - `/weather/get`, `/weather/save`, `/weather/autoloc` – Weather prefs & IP geo (ip‑api.com).
  - Captive helpers: `/generate_204`, `/hotspot-detect.html`, `/captiveportal`, etc.
- Preferences namespaces:
  - `wifi` → `ssid`, `pass`
  - `weather` → `enabled`, `units` (“F”/“C”), `lat`, `lon`, `refresh` (min), `name`

### Battery gauge (LC709203F)
- Shares the same I²C bus, default address **0x0B**.
- On‑screen value is **smoothed** to avoid boot‑time jitter (warm‑up + exponential smoothing).
- If percent is invalid but voltage is good, a rough voltage→% curve is used as a fallback.
- Optional APA config via build defines:
  - `LC709203F_APA_2000MAH`, `LC709203F_APA_2200MAH`, `LC709203F_APA_2500MAH`, etc.

---

## 🧪 Building

### Arduino IDE
1. Install **ESP32** board support (ESP32‑S3 enabled).
2. Libraries:
   - **U8g2**
   - **ESP Async WebServer** (+ **AsyncTCP** for ESP32)
   - **Adafruit LC709203F**
   - **ArduinoJson**
3. Open the project, confirm I²C pins and OLED address (0x3D), then upload.

---

## 🚀 First Boot

1. Power the device. It boots into **AP+STA** and opens the portal:
   - SSID: **Type D Wireless Display Setup**
2. Visit any page (captive portal) or go to `http://192.168.4.1/`.
3. Select your Wi‑Fi, click **Connect & Save**.
4. (Optional) On the Weather card:
   - Check **Enable weather**, choose units, set **Refresh** minutes.
   - Enter **lat/lon** or click **Auto‑detect**.
   - Save.
5. When connected to your LAN, try `http://typeddisp.local/`.

> You should see the **boot logo** until the first UDP packets arrive.

---

## 🔍 Troubleshooting

- **Only boot logo, never changes**  
  No UDP is being received. Verify your sender IP/ports (see `udp_typed.h`) and that the device is on the same network.
- **Goes to “Sleeping…” even though I’m sending data**  
  Make sure the sender actually emits packets within 5 minutes and that they match one of the **A/B/C** ports. The inactivity logic is based on “last packet time”, not packet contents.
- **Weather never shows**  
  Enable it in the portal, provide a valid lat/lon (or use Auto‑detect), and ensure the device has Internet access.
- **Battery widget doesn’t appear**  
  LC709203F not detected on I²C 0x0B, or warm‑up not finished. Check wiring and that the APA define matches your pack (optional but recommended).
- **Display looks mirrored/rotated**  
  Use `u8g2.setFlipMode(1)` or rotate in the constructor variant if needed.

---

## 🗺️ Roadmap

- On‑device OTA (web‑update) page.
- Scroll‑in place for long resolution strings.
- Optional iconography for weather (we currently use text‑only for reliability).

---

## 📜 License & Credits

- Code: MIT (unless otherwise noted in source files).
- Weather: https://open-meteo.com (no key). IP geolocation: https://ip-api.com.
- Fonts & display driver: https://github.com/olikraus/u8g2
- Fuel gauge: Adafruit LC709203F library.

---

## 🙌 Contributing

PRs welcome! Please include:
- Your board/panel variant and wiring
- Repro steps or packet samples if you’re touching UDP
- Before/after screenshots for UI tweaks
