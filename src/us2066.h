#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal US2066 (20x4) I2C driver — character OLED controller
// - Default I2C address commonly 0x3C or 0x3D (depends on SA0 wiring).
// - Provides HD44780-like API: clear, home, setCursor, print/write, displayOn, setContrast.
// - Designed to be lightweight and safe on ESP32 (no Wire.begin() inside by default).

#ifndef US2066_DEFAULT_ADDR
#define US2066_DEFAULT_ADDR 0x3C
#endif

class US2066 {
public:
  US2066();

  // Initialize. Does NOT call Wire.begin(); pass your Wire (default &Wire)
  // cols/rows default to 20x4; you can change if your module differs.
  bool begin(TwoWire* w = &Wire, uint8_t i2c_addr = US2066_DEFAULT_ADDR,
             uint8_t cols = 20, uint8_t rows = 4);

  // Quick presence check (0 == OK)
  bool ping() const;

  // Basic API
  void clear();            // 1.6ms
  void home();             // 1.6ms
  void setCursor(uint8_t col, uint8_t row);
  void displayOn(bool on);
  void setContrast(uint8_t level); // 0..255 (mapped to controller)
  void noCursor();          // display ON, cursor OFF, blink OFF
  void cursor();            // display ON, cursor ON, blink OFF
  void blink();             // display ON, cursor ON, blink ON

  // Print helpers
  size_t write(uint8_t c);
  size_t write(const char* s);
  size_t write(const String& s);
  void   writeLine(uint8_t row, const char* s, bool padToWidth = true); // write + pad/trim
  void   clearLine(uint8_t row);

  // Custom glyphs (HD44780-style 5x8; idx 0..7), pattern[8] 5 LSBs used
  void createChar(uint8_t idx, const uint8_t pattern[8]);

  // Accessors
  inline uint8_t cols() const { return _cols; }
  inline uint8_t rows() const { return _rows; }
  inline uint8_t addr() const { return _addr; }

private:
  TwoWire* _wire;
  uint8_t  _addr;
  uint8_t  _cols, _rows;

  // I2C control bytes (US2066)
  static constexpr uint8_t CB_CMD  = 0x00; // next byte is command
  static constexpr uint8_t CB_DATA = 0x40; // next bytes are data

  // Timing helpers
  static void _delayShort();  // ~30–50us
  static void _delayLong();   // ~2ms (for clear/home)

  // Low-level
  bool _cmd(uint8_t c);
  bool _cmd2(uint8_t c1, uint8_t c2);
  bool _data(const uint8_t* b, size_t n);
  bool _dataByte(uint8_t b);

  // Init sequence for 4-line OLED (US2066 extended)
  bool _initSequence();

  // Addressing
  uint8_t _ddramBase(uint8_t row) const; // base for 20x4 -> {0x00,0x20,0x40,0x60}
};
