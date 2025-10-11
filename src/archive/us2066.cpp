#include "us2066.h"

// Notes / References:
// - US2066 uses I2C control prefix: 0x00 for command, 0x40 for data.
// - 20x4 DDRAM row bases commonly map to 0x00, 0x20, 0x40, 0x60.
// - Some modules need the “extended OLED” init sequence (RE/SD toggles).
//   The sequence below is a conservative set known to work on US2066-based
//   20x4 Newhaven-style modules.

US2066::US2066()
: _wire(&Wire), _addr(US2066_DEFAULT_ADDR), _cols(20), _rows(4)
{}

bool US2066::begin(TwoWire* w, uint8_t i2c_addr, uint8_t cols, uint8_t rows) {
  _wire = w ? w : &Wire;
  _addr = i2c_addr;
  _cols = cols;
  _rows = rows;

  // quick ping so we can early-out if not present
  if (!ping()) return false;

  return _initSequence();
}

bool US2066::ping() const {
  // NOTE: do not call Wire.begin() here; user should have initialized already.
  _wire->beginTransmission(_addr);
  return (_wire->endTransmission() == 0);
}

void US2066::_delayShort() { delayMicroseconds(40); }
void US2066::_delayLong()  { delay(2); } // clear/home needs >1.52ms

bool US2066::_cmd(uint8_t c) {
  _wire->beginTransmission(_addr);
  _wire->write(CB_CMD);
  _wire->write(c);
  if (_wire->endTransmission() != 0) return false;
  // Small guard; some commands need a short pause
  _delayShort();
  return true;
}

bool US2066::_cmd2(uint8_t c1, uint8_t c2) {
  _wire->beginTransmission(_addr);
  _wire->write(CB_CMD);
  _wire->write(c1);
  _wire->write(c2);
  if (_wire->endTransmission() != 0) return false;
  _delayShort();
  return true;
}

bool US2066::_data(const uint8_t* b, size_t n) {
  // Chunk writes to avoid long I2C bursts; 16 bytes is safe for most stacks
  const size_t CHUNK = 16;
  size_t off = 0;
  while (off < n) {
    size_t cnt = (n - off > CHUNK) ? CHUNK : (n - off);
    _wire->beginTransmission(_addr);
    _wire->write(CB_DATA);
    _wire->write(b + off, cnt);
    if (_wire->endTransmission() != 0) return false;
    off += cnt;
  }
  return true;
}

bool US2066::_dataByte(uint8_t b) {
  _wire->beginTransmission(_addr);
  _wire->write(CB_DATA);
  _wire->write(b);
  return (_wire->endTransmission() == 0);
}

bool US2066::_initSequence() {
  // Conservative US2066 4-line OLED init (extended instruction set)
  // Many vendor notes use a similar order. Keep short delays after block writes.

  // Function set (RE=1) – select extended instruction set
  // 0x2A: DL=1 (8-bit), N=1 (2-line), RE=1 (Extended), IS=0
  if (!_cmd(0x2A)) return false;

  // Internal VDD regulator control via 0x71 then data 0x5C (per NHD app note)
  if (!_cmd(0x71)) return false;
  if (!_dataByte(0x5C)) return false; // function data
  _delayShort();

  // Function set (RE=0)
  if (!_cmd(0x28)) return false;

  // Display OFF
  if (!_cmd(0x08)) return false;

  // RE=1 again, then enter OLED command set (SD=1)
  if (!_cmd(0x2A)) return false;
  if (!_cmd(0x79)) return false; // OLED command set enabled

  // Set display clock divide (typical)
  if (!_cmd2(0xD5, 0x70)) return false;

  // Exit OLED command set (SD=0)
  if (!_cmd(0x78)) return false;

  // Extended function set: 4-line mode (0x09)
  if (!_cmd(0x09)) return false;

  // Entry mode: increment, no shift
  if (!_cmd(0x06)) return false;

  // Optional: ROM selection (0x72 + data), often 0x00 for CGROM A
  if (!_cmd(0x72)) return false;
  if (!_dataByte(0x00)) return false;

  // Re-enter OLED command set to set panel specifics
  if (!_cmd(0x2A)) return false;
  if (!_cmd(0x79)) return false;

  // Segment pins hardware config (example)
  if (!_cmd2(0xDA, 0x10)) return false;

  // Set VSL (external/internal) – example uses 0x00
  if (!_cmd2(0xDC, 0x00)) return false;

  // Contrast (default mid-high)
  if (!_cmd2(0x81, 0x7F)) return false;

  // Phase length (pre-charge), typical
  if (!_cmd2(0xD9, 0xF1)) return false;

  // VCOMH deselect level
  if (!_cmd2(0xDB, 0x40)) return false;

  // Exit OLED command set
  if (!_cmd(0x78)) return false;

  // Back to basic instruction set
  if (!_cmd(0x28)) return false;

  // Clear, home, display ON
  clear();
  if (!_cmd(0x80)) return false; // DDRAM addr=0
  displayOn(true);
  return true;
}

void US2066::clear() {
  _cmd(0x01);
  _delayLong();
}

void US2066::home() {
  _cmd(0x02);
  _delayLong();
}

uint8_t US2066::_ddramBase(uint8_t row) const {
  // Typical US2066 20x4 mapping
  static const uint8_t base[4] = { 0x00, 0x20, 0x40, 0x60 };
  if (row >= _rows) row = _rows - 1;
  if (_rows >= 4) return base[row];
  // Fallbacks for 1/2/3 rows (rare on US2066, but harmless)
  if (_rows == 1) return 0x00;
  if (_rows == 2) return row ? 0x40 : 0x00;
  // 3-line: use 0x00, 0x20, 0x40
  if (row == 0) return 0x00;
  if (row == 1) return 0x20;
  return 0x40;
}

void US2066::setCursor(uint8_t col, uint8_t row) {
  if (col >= _cols) col = _cols - 1;
  uint8_t addr = (uint8_t)(_ddramBase(row) + col);
  _cmd(0x80 | addr); // Set DDRAM address
}

void US2066::displayOn(bool on) {
  // Display ON/OFF control: 0x08 | D | C | B
  // D=1 display on; cursor/blink keep previous unless changed
  if (on) _cmd(0x0C); // D=1, C=0, B=0
  else    _cmd(0x08); // D=0
}

void US2066::noCursor() { _cmd(0x0C); } // display on, cursor off, blink off
void US2066::cursor()   { _cmd(0x0E); } // display on, cursor on,  blink off
void US2066::blink()    { _cmd(0x0F); } // display on, cursor on,  blink on

void US2066::setContrast(uint8_t level) {
  // US2066 contrast via OLED command set (81h, value)
  // Enter/exit SD carefully around the write.
  _cmd(0x2A);     // RE=1
  _cmd(0x79);     // SD=1
  _cmd2(0x81, level); // contrast
  _cmd(0x78);     // SD=0
  _cmd(0x28);     // RE=0
}

size_t US2066::write(uint8_t c) {
  // Basic ASCII; for UTF-8 callers, pre-convert to glyphs supported by CGROM.
  if (_dataByte(c)) return 1;
  return 0;
}

size_t US2066::write(const char* s) {
  if (!s) return 0;
  size_t n = strlen(s);
  if (n == 0) return 0;
  _data(reinterpret_cast<const uint8_t*>(s), n);
  return n;
}

size_t US2066::write(const String& s) {
  return write(s.c_str());
}

void US2066::writeLine(uint8_t row, const char* s, bool padToWidth) {
  setCursor(0, row);
  if (!s) s = "";
  // Write up to _cols chars, then pad spaces if requested
  for (uint8_t i=0; i<_cols; ++i) {
    char ch = s[i];
    if (ch == '\0') {
      if (padToWidth) {
        while (i++ < _cols) write(' ');
      }
      return;
    }
    write((uint8_t)ch);
  }
}

void US2066::clearLine(uint8_t row) {
  setCursor(0, row);
  for (uint8_t i=0; i<_cols; ++i) write(' ');
  setCursor(0, row);
}

void US2066::createChar(uint8_t idx, const uint8_t pattern[8]) {
  // HD44780-compatible CGRAM (0..7), each row uses lower 5 bits.
  idx &= 0x07;
  _cmd(0x40 | (idx << 3)); // Set CGRAM address
  for (uint8_t i=0; i<8; ++i) {
    _dataByte(pattern ? (pattern[i] & 0x1F) : 0x00);
  }
  // restore DDRAM address (host should setCursor() after this)
}
