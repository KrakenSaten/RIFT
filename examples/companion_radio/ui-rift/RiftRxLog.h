#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// Every packet the radio hears, and every packet it sends.
//
// Receive-only until now, which left the most basic field question unanswerable on the
// device: did my advert actually go out. A transmit that failed before reaching the air
// was invisible entirely - and that is a different fault from one that was sent and
// never acknowledged, which the message log already distinguishes.
//
// Both directions come from hooks upstream already declares and leaves empty:
// Dispatcher::logTx() and logTxFail(). Nothing here diverges from upstream to get them.
//
// The event log records things worth a sentence. This records traffic, which is a
// different volume by orders of magnitude - a busy mesh would push everything else
// out of a shared ring within seconds - so it gets its own, and a much smaller
// record per entry.
//
// Nothing is formatted here. The capture runs inside logRxRaw(), on the packet path,
// and there is no watchdog on the main loop: a snprintf per packet is work that does
// not need to happen until somebody is looking. Six fields are copied and the
// rendering does the rest.
//
// Deliberately not persisted. It describes the last few minutes of radio activity,
// which is worth nothing after a reboot, and writing to flash on the packet path is
// the opposite of what this is for.

#define RIFT_RX_LOG_LINES 96

// Direction, and for a transmit whether it reached the air.
#define RIFT_AIR_RX     0
#define RIFT_AIR_TX     1
#define RIFT_AIR_TXFAIL 2

struct RiftRxLog {
  struct Entry {
    uint32_t at_ms;
    // Signal on a receive; air time in milliseconds on a transmit.
    //
    // Reused rather than widened. RSSI and SNR describe a signal arriving and mean
    // nothing about one leaving, so a transmit row would have carried two dead
    // columns - and air time is the thing a transmit row should say instead, since it
    // is what the duty cycle is spent on. Two bytes hold 65 seconds, and no packet on
    // any spreading factor comes close.
    union {
      struct {
        int8_t snr4;     // SNR * 4, as the radio reports it
        int8_t rssi;
      } rx;
      uint16_t air_ms;
    };
    uint8_t  header;     // route type in bits 0-1, payload type in bits 2-5
    uint8_t  path_len;   // Packet's encoding: hash count and hash size
    uint8_t  len;        // whole packet, capped at 255 which no packet exceeds
    uint8_t  dir;        // RIFT_AIR_*
  };
  Entry lines[RIFT_RX_LOG_LINES];
  int head = RIFT_RX_LOG_LINES - 1;
  int count = 0;
  uint32_t total = 0;    // every packet ever heard, not just the ones still in the ring

  // Counted separately, because "how much have I heard" and "how much have I said"
  // are different questions and a combined figure answers neither. A node that has
  // heard 4000 packets and sent 3 is in a very different situation from one that has
  // sent 400.
  uint32_t total_tx = 0;

  Entry* alloc(uint8_t dir, uint8_t header, uint8_t path_len, int len) {
    head = (head + 1) % RIFT_RX_LOG_LINES;
    if (count < RIFT_RX_LOG_LINES) count++;
    if (dir == RIFT_AIR_RX) {
      if (total < 0xFFFFFFFFu) total++;
    } else if (total_tx < 0xFFFFFFFFu) {
      total_tx++;
    }
    Entry* e = &lines[head];
    e->at_ms = (uint32_t) millis();
    e->dir = dir;
    e->header = header;
    e->path_len = path_len;
    e->len = (uint8_t) (len > 255 ? 255 : (len < 0 ? 0 : len));
    return e;
  }

  void add(float snr, float rssi, uint8_t header, uint8_t path_len, int len) {
    Entry* e = alloc(RIFT_AIR_RX, header, path_len, len);
    // clamped rather than cast: a bad reading should not wrap into a plausible one
    int s4 = (int) (snr * 4.0f);
    e->rx.snr4 = (int8_t) (s4 < -128 ? -128 : (s4 > 127 ? 127 : s4));
    int r = (int) rssi;
    e->rx.rssi = (int8_t) (r < -128 ? -128 : (r > 127 ? 127 : r));
  }

  // A failed transmit still records its air time, and that number means something
  // different: how long the radio was held, not how long it was transmitting. The
  // send-timeout path in Dispatcher never adds to the air-time total at all, so a
  // timed-out row reads 0 rather than an invented figure.
  void addTx(bool ok, uint32_t air_ms, uint8_t header, uint8_t path_len, int len) {
    Entry* e = alloc(ok ? RIFT_AIR_TX : RIFT_AIR_TXFAIL, header, path_len, len);
    e->air_ms = (uint16_t) (air_ms > 65535u ? 65535u : air_ms);
  }

  // back == 0 is the newest
  const Entry* peek(int back) const {
    if (back < 0 || back >= count) return NULL;
    return &lines[(head - back + RIFT_RX_LOG_LINES * 2) % RIFT_RX_LOG_LINES];
  }
};

// One instance across both translation units: MyMesh writes it from the packet path,
// the UI reads it. A function-local static in an inline function is a single object,
// which a file-scope definition in a header would not be.
inline RiftRxLog& riftRxLog() {
  static RiftRxLog log;
  return log;
}
