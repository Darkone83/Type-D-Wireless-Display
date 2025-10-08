# Type-D Wireless Display

<div align=center>
  <img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/DC%20logo.png">
  <p></p>
  <img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/Disp.jpg" height=400>
</div>
<p></p>

A tiny OLED “status screen” for original Xbox setups (and other sources), driven over Wi‑Fi/UDP.  
Shows a main system summary, a dense second page, a health page, and an optional weather page, and insignia leaderboard page.
Designed for the Waveshare 2.42″ SSD1309 I²C panel (128×64), with added experimental support for US2066 OLED displays.
A Type D Expansion unit is required for these displays to properly work and display data. See more info <a href="https://github.com/Darkone83/Type-D/tree/main/EXP%20Src">Here</a>. To display the App name, the XBMC4Gamers script for the Type D Expansion unit is required.

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

---

## 🧰 Required Hardware

### Desktop build
- **ESP32‑S3**: Waveshare ESP32-S3 Zero or compatable. <a href="https://www.aliexpress.us/item/3256808233319699.html?spm=a2g0o.order_list.order_list_main.5.53061802JTioD2&gatewayAdapt=glo2usa">AliExpress (Clone)</a>, <a href="https://www.amazon.com/dp/B0CHYHGYRH?th=1">Amazon</a>.
- **Waveshare 2.42″ OLED**. <a href="https://www.waveshare.com/2.42inch-oled-module.htm">WaveShare</a>, <a href="https://www.amazon.com/dp/B0CM3RHMGP">Amazon</a>.
- *(Optional)* **LC709203F** fuel gauge (Adafruit breakout works great). <a href="https://www.adafruit.com/product/4712">Adafruit</a>.
- *(Optional)* LiPo pack if you want the battery widget to show %/voltage. <a href="https://www.adafruit.com/product/1781">Adafruit</a>.
- *Power Switches*: <a href="https://www.amazon.com/dp/B0DR2DPW59?th=1">Amazon</a>
- *Screws*: 4x 2mm x 4mm Pan head screws.

### Controller port build
- **ESP32‑S3**: Waveshare ESP32-S3 Zero or compatable. <a href="https://www.aliexpress.us/item/3256808233319699.html?spm=a2g0o.order_list.order_list_main.5.53061802JTioD2&gatewayAdapt=glo2usa">AliExpress (Clone)</a>, <a href="https://www.amazon.com/dp/B0CHYHGYRH?th=1">Amazon</a>.
- **Waveshare 2.42″ OLED**. <a href="https://www.waveshare.com/2.42inch-oled-module.htm">WaveShare</a>, <a href="https://www.amazon.com/dp/B0CM3RHMGP">Amazon</a>.
- **2 Caps**: 1x 100uF, 1X 10uF
- **Sacrificial OG XBOX dongle**: Needed to actually interface with the xbox controller port for power. <a href="https://www.aliexpress.us/item/3256803253950796.html?spm=a2g0o.productlist.main.9.336eCivDCivDaG&algo_pvid=997e7e34-7d89-4b7c-a748-d8c6e39694a0&algo_exp_id=997e7e34-7d89-4b7c-a748-d8c6e39694a0-8&pdp_ext_f=%7B%22order%22%3A%22648%22%2C%22eval%22%3A%221%22%2C%22fromPage%22%3A%22search%22%7D&pdp_npi=6%40dis%21USD%212.74%210.99%21%21%212.74%210.99%21%402101c59817585859608863421ecf27%2112000025800335880%21sea%21US%210%21ABX%211%210%21n_tag%3A-29910%3Bd%3A7fd4d803%3Bm03_new_user%3A-29895%3BpisId%3A5000000174448336&curPageLogUid=zk6ZlcgXIpxN&utparam-url=scene%3Asearch%7Cquery_from%3A%7Cx_object_id%3A1005003440265548%7C_p_origin_prod%3A">AliExpress</a>
- **Picoblade Wiring kit:** <a href="https://www.amazon.com/1-25mm-Connectors-Pre-Crimped-Pixhawk-Silicone/dp/B07S18D3RN/ref=sr_1_7?crid=86ODWZP1A6T0&dib=eyJ2IjoiMSJ9.YBe0_2n5_7xBVWTYGBc9tXnNvlqkj8BkiQFtvQS_5xsOBhFz5jWBBLEPUmqo5PFwdmBq-ZT7zJx7xt0ffwcumgq2dBEwWinauZjDotWsJx1PyXUTV-dKz7uScUxa7XOcttjNMJG4vlD9akdLiPiEdDXgOwBG9r99-e_R1khgKFeKvNKfVOZ3MDGtXD4M7O_LMz3PpaeEFEnOSYBqae5WC96CdWgnCu_NtYBJDZYIDBCH5y-03oIn6juerycC7Q2myOHsXYvkQyvpoPPh7RxT1KWXOLpo2cxJIiwgCiGnyAM.2FdaC2BOccSLj87AAR_PSk1NdEWa2JEhb1aTgxUNwjI&dib_tag=se&keywords=picoblade+1.25&qid=1758587206&sprefix=picoblade+%2Caps%2C205&sr=8-7">Amazon</a>, tou will need to the the 7p connectors.

---

## Purchase options:

Controller port kit: <a href="https://www.darkonecustoms.com/store/p/type-d-wireless-oled-kit">Darkone Customs</a>

---


### Wiring (I²C bus shared by OLED + LC709203F)

| Signal | ESP32‑S3 Pin | OLED | LC709203F |
|-------:|:-------------|:-----|:----------|
| SDA    | **GPIO 6**   | SDA  | SDA       |
| SCL    | **GPIO 7**   | SCL  | SCL       |
| RST    | **GPIO 9**   | RST  |           |
| 3V3    | 3V3          | VCC  | VIN       |
| GND    | GND          | GND  | GND       |


---


## 🧪 Building


### Hardware:

#### Preparation:

Display needs the 2 0ohm resistors swapped from SPI position to the I2C position. If installing in the controller port shell, the pin header will need to be desoldered


#### Desktop Build:

For the desktop case, it's recommended to use a 18650 cell; other cells may work, but haven't been tested.

Images:

<img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/Desktop%201.jpg" height=400 width=400><img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/Desktop%202.jpg" height=400 width=400>

#### Controller Port:

Use some double-sided adhesive to assist with securing the display to the case. You will also need the adhesive for either the PCB installation or bare devboard installation.

Keep wire runs as short as possible. For the controller port connector, ensure it's flush with the rear of the case. Clearance is tight; test fit often during installation. 

If installing with a bare dev board, review the images for capacitor installation. The 100uF cap should be attached to the dev board, and the 10uF cap should be placed on the OLED's power pins. 

If installing the PCB, you will likely need to create your own wiring harness. View pinouts below

Images:

<img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/Controller%201.jpg" height=400 width=400><img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/Controller%202.jpg" height=400 width=400>

Note: Images shown are from development builds

### Pinouts:

<img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/CN1.png">


---

## Software prep:

### Arduino IDE
1. Install **ESP32** board support (ESP32‑S3 enabled).
2. Libraries:
   - **U8g2**
   - **ESP Async WebServer** (+ **AsyncTCP** for ESP32)
   - **Adafruit LC709203F**
   - **ArduinoJson**
3. Open the project, confirm I²C pins, then upload.

---

## 🚀 First Boot

<img src="https://github.com/Darkone83/Type-D-Wireless-Display/blob/main/images/Setup.png" height=400>

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

---

## 🗺️ Roadmap

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
