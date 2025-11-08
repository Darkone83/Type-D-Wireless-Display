#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// -------- Config (override via -D or before include) --------
#ifndef UDP_TYPED_MAX_PAYLOAD
#define UDP_TYPED_MAX_PAYLOAD 1024      // bytes stored per packet (rest is clipped)
#endif

#ifndef UDP_TYPED_DEFAULT_PORT_A
#define UDP_TYPED_DEFAULT_PORT_A 50504  // Type-D telemetry (primary)
#endif
#ifndef UDP_TYPED_DEFAULT_PORT_B
#define UDP_TYPED_DEFAULT_PORT_B 50505  // EXT
#endif
#ifndef UDP_TYPED_DEFAULT_PORT_C
#define UDP_TYPED_DEFAULT_PORT_C 50506  // EEPROM text
#endif

#ifndef UDP_TYPED_QUEUE_DEPTH
#define UDP_TYPED_QUEUE_DEPTH 12        // ring buffer size for incoming packets
#endif

namespace TypeDUDP {

struct Packet {
  uint32_t   ts_ms = 0;                 // millis() when received
  IPAddress  ip;                         // sender IP
  uint16_t   port = 0;                   // legacy alias of src_port
  uint16_t   src_port = 0;               // sender source port (remotePort)
  uint16_t   dst_port = 0;               // local socket port we received on (50504/50505/50506)
  size_t     rx_len = 0;                 // bytes actually stored (<= MAX_PAYLOAD)
  size_t     clipped = 0;                // bytes dropped if > MAX_PAYLOAD
  char       data[UDP_TYPED_MAX_PAYLOAD + 1]; // payload (NUL-terminated)
};

// Lifecycle
void begin(uint16_t portA = UDP_TYPED_DEFAULT_PORT_A,
           uint16_t portB = UDP_TYPED_DEFAULT_PORT_B,
           uint16_t portC = UDP_TYPED_DEFAULT_PORT_C);

void end();
void loop();

// Debug control
void setDebug(bool enable);
bool debugEnabled();

// Legacy/simple accessors
bool hasPacket();                        // true if we've *ever* received a packet since begin()
uint32_t lastSeenMs();                   // millis() of last-received packet (0 if none)
bool isAlive(uint32_t timeout_ms);       // lastSeen within timeout_ms
uint32_t packetCount();                  // total packets since begin()

bool armed();                            // armed (waiting for Wi-Fi)
bool started();                          // sockets are bound

// Legacy: latest packet snapshot (updated on every receive)
const Packet& last();

// ---------- New queue API (recommended) ----------
bool available();                        // any packets queued?
size_t pendingCount();                   // number of packets waiting
bool next(Packet& out);                  // pop oldest queued packet into 'out'

} // namespace TypeDUDP
