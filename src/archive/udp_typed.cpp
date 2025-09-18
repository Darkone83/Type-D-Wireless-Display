#include "udp_typed.h"
#include <WiFi.h>    // for WiFi.status()
#include <Arduino.h> // for millis()

namespace TypeDUDP {

enum class Mode : uint8_t { OFF = 0, ARMED, STARTED };

static WiFiUDP s_udpA, s_udpB, s_udpC;
static bool    s_udpA_on = false, s_udpB_on = false, s_udpC_on = false;

// "Ever saw a packet since begin()" flag and core timing stats
static volatile bool     s_ever     = false;
static volatile uint32_t s_pktCount = 0;
static volatile uint32_t s_lastSeen = 0;

static Packet  s_last;                        // latest packet snapshot (legacy)
static bool    s_debug = false;

static Mode    s_mode = Mode::OFF;
static uint16_t s_portA = UDP_TYPED_DEFAULT_PORT_A;
static uint16_t s_portB = UDP_TYPED_DEFAULT_PORT_B;
static uint16_t s_portC = UDP_TYPED_DEFAULT_PORT_C;

// -------- Ring buffer for packets --------
static Packet  s_q[UDP_TYPED_QUEUE_DEPTH];
static uint8_t s_q_head = 0;   // next write
static uint8_t s_q_tail = 0;   // next read

static inline uint8_t q_next(uint8_t v) { return (uint8_t)((v + 1) % UDP_TYPED_QUEUE_DEPTH); }
static inline bool    q_full()  { return q_next(s_q_head) == s_q_tail; }
static inline bool    q_empty() { return s_q_head == s_q_tail; }
static inline size_t  q_count() {
  int diff = (int)s_q_head - (int)s_q_tail;
  if (diff < 0) diff += UDP_TYPED_QUEUE_DEPTH;
  return (size_t)diff;
}

static bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

static inline void ipToStr(const IPAddress& ip, char* out, size_t cap) {
  if (cap < 8) return;
  snprintf(out, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void bindIfReady() {
  if (s_mode != Mode::ARMED) return;
  if (!wifiConnected()) return;

  s_udpA_on = s_portA ? s_udpA.begin(s_portA) : false;
  s_udpB_on = s_portB ? s_udpB.begin(s_portB) : false;
  s_udpC_on = s_portC ? s_udpC.begin(s_portC) : false;

  s_mode = Mode::STARTED;

  if (s_debug) {
    Serial.printf("[TypeDUDP] started  A:%u(%s)  B:%u(%s)  C:%u(%s)\n",
                  (unsigned)s_portA, s_udpA_on ? "on" : "off",
                  (unsigned)s_portB, s_udpB_on ? "on" : "off",
                  (unsigned)s_portC, s_udpC_on ? "on" : "off");
  }
}

static void unbindIfDown() {
  if (s_mode != Mode::STARTED) return;
  if (wifiConnected()) return;

  if (s_udpA_on) { s_udpA.stop(); s_udpA_on = false; }
  if (s_udpB_on) { s_udpB.stop(); s_udpB_on = false; }
  if (s_udpC_on) { s_udpC.stop(); s_udpC_on = false; }

  s_mode = Mode::ARMED; // re-arm for the next reconnect

  if (s_debug) {
    Serial.println("[TypeDUDP] wifi down -> sockets closed, re-armed");
  }
}

// Push packet into ring; if full, drop the oldest (advance tail)
static void queuePush(const Packet& pk) {
  if (q_full()) {
    // drop oldest to keep the most recent data
    s_q_tail = q_next(s_q_tail);
    if (s_debug) Serial.println("[TypeDUDP] queue full -> dropped oldest");
  }
  s_q[s_q_head] = pk;
  s_q_head = q_next(s_q_head);
}

static void drainSocket(WiFiUDP& udp, uint16_t dst_hint) {
  // Read ALL pending packets from this socket
  int pktSize;
  while ((pktSize = udp.parsePacket()) > 0) {
    Packet pk;
    size_t want = (size_t)pktSize;
    if (want > UDP_TYPED_MAX_PAYLOAD) {
      pk.clipped = want - UDP_TYPED_MAX_PAYLOAD;
      want = UDP_TYPED_MAX_PAYLOAD;
    } else {
      pk.clipped = 0;
    }

    int n = udp.read((uint8_t*)pk.data, want);
    if (n < 0) n = 0;
    pk.data[n] = '\0';
    pk.rx_len   = (size_t)n;
    pk.ip       = udp.remoteIP();
    pk.src_port = udp.remotePort();
    pk.port     = pk.src_port;     // legacy alias
    pk.dst_port = dst_hint;        // reliable classification
    pk.ts_ms    = millis();

    // Update legacy stats
    s_last     = pk;
    s_ever     = true;
    s_lastSeen = pk.ts_ms;
    s_pktCount++;

    // Enqueue for consumers
    queuePush(pk);

    if (s_debug) {
      char ipbuf[20];
      ipToStr(pk.ip, ipbuf, sizeof(ipbuf));
      Serial.printf("[TypeDUDP] %lu  src=%s:%u dst=%u  len=%u",
                    (unsigned long)pk.ts_ms, ipbuf, (unsigned)pk.src_port,
                    (unsigned)pk.dst_port, (unsigned)pktSize);
      if (pk.clipped) Serial.printf(" (clipped %u)", (unsigned)pk.clipped);
      Serial.print("  data=\"");
      const char* p = pk.data;
      size_t shown = 0;
      while (*p && shown < 96) {
        char c = *p++;
        if (c >= 32 && c <= 126) Serial.write(c);
        else if (c == '\n') Serial.print("\\n");
        else if (c == '\r') Serial.print("\\r");
        else if (c == '\t') Serial.print("\\t");
        else Serial.print(".");
        shown++;
      }
      if (pk.rx_len > shown) Serial.print("...");
      Serial.println("\"");
    }
  }
}

// ---------- API ----------
void begin(uint16_t portA, uint16_t portB, uint16_t portC) {
  // Store desired ports; do NOT bind yet unless Wi-Fi is already connected.
  s_portA = portA;
  s_portB = portB;
  s_portC = portC;

  s_ever     = false;
  s_pktCount = 0;
  s_lastSeen = 0;
  s_last     = Packet(); // reset

  // reset queue
  s_q_head = s_q_tail = 0;

  s_mode = Mode::ARMED;
  if (wifiConnected()) {
    bindIfReady();
  } else if (s_debug) {
    Serial.println("[TypeDUDP] armed (waiting for Wi-Fi)");
  }
}

void end() {
  if (s_udpA_on) { s_udpA.stop(); s_udpA_on = false; }
  if (s_udpB_on) { s_udpB.stop(); s_udpB_on = false; }
  if (s_udpC_on) { s_udpC.stop(); s_udpC_on = false; }
  s_mode = Mode::OFF;
  if (s_debug) Serial.println("[TypeDUDP] stopped");
}

void loop() {
  // Handle state transitions first
  if (s_mode == Mode::ARMED) bindIfReady();
  else if (s_mode == Mode::STARTED) unbindIfDown();

  // Drain all sockets when running
  if (s_mode == Mode::STARTED) {
    if (s_udpA_on) drainSocket(s_udpA, s_portA);
    if (s_udpB_on) drainSocket(s_udpB, s_portB);
    if (s_udpC_on) drainSocket(s_udpC, s_portC);
  }
}

void setDebug(bool enable) {
  s_debug = enable;
  if (s_debug) Serial.println("[TypeDUDP] debug = ON");
}

bool debugEnabled() { return s_debug; }

// NEW: pending packets in the queue (use this in loops)
bool hasPacket() { return !q_empty(); }

// NEW: retain the old meaning — have we ever seen any packet since begin()?
bool everReceived() { return s_ever; }

uint32_t lastSeenMs() { return s_lastSeen; }

bool isAlive(uint32_t timeout_ms) {
  uint32_t now = millis();
  return s_lastSeen != 0 && (uint32_t)(now - s_lastSeen) <= timeout_ms;
}

uint32_t packetCount() { return s_pktCount; }

bool armed()   { return s_mode == Mode::ARMED; }
bool started() { return s_mode == Mode::STARTED; }

const Packet& last() { return s_last; }

// -------- Queue API --------
bool available() { return !q_empty(); }

size_t pendingCount() { return q_count(); }

bool next(Packet& out) {
  if (q_empty()) return false;
  out = s_q[s_q_tail];
  s_q_tail = q_next(s_q_tail);
  return true;
}

// Optional convenience: drop any queued backlog
void flush() { s_q_tail = s_q_head; }

} // namespace TypeDUDP
