#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// Every packet the radio hears, as it hears it.
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

struct RiftRxLog {
  struct Entry {
    uint32_t at_ms;
    int8_t   snr4;       // SNR * 4, as the radio reports it
    int8_t   rssi;
    uint8_t  header;     // route type in bits 0-1, payload type in bits 2-5
    uint8_t  path_len;   // Packet's encoding: hash count and hash size
    uint8_t  len;        // whole packet, capped at 255 which no packet exceeds
  };
  Entry lines[RIFT_RX_LOG_LINES];
  int head = RIFT_RX_LOG_LINES - 1;
  int count = 0;
  uint32_t total = 0;    // every packet ever heard, not just the ones still in the ring

  void add(float snr, float rssi, uint8_t header, uint8_t path_len, int len) {
    head = (head + 1) % RIFT_RX_LOG_LINES;
    if (count < RIFT_RX_LOG_LINES) count++;
    if (total < 0xFFFFFFFFu) total++;
    Entry* e = &lines[head];
    e->at_ms = (uint32_t) millis();
    // clamped rather than cast: a bad reading should not wrap into a plausible one
    int s4 = (int) (snr * 4.0f);
    e->snr4 = (int8_t) (s4 < -128 ? -128 : (s4 > 127 ? 127 : s4));
    int r = (int) rssi;
    e->rssi = (int8_t) (r < -128 ? -128 : (r > 127 ? 127 : r));
    e->header = header;
    e->path_len = path_len;
    e->len = (uint8_t) (len > 255 ? 255 : (len < 0 ? 0 : len));
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
