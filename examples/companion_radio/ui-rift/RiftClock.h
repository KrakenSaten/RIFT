#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The clock, disciplined by the mesh.
//
// The T-Deck has no battery-backed RTC. After a power-on reset - which is what a
// flash ends with - ESP32RTCClock::begin sets the time to a fixed day in 2024,
// and BaseChatMesh::bootstrapRTCfromContacts then moves it to the newest
// timestamp in the stored contact table plus one second. That is the moment
// the last advert was heard before the reset, so the clock comes up late by
// however long the device was off or being flashed, and a standalone node has
// nothing to correct it: no companion app, and a GPS fix only when there is one.
// "The clock is often wrong, and late" was the report, and this is why.
//
// Every advert carries the sender's clock, and the contact record keeps it next
// to our own clock at the moment of receipt (last_advert_timestamp and lastmod),
// so each advert is one measurement of how far we are from that node. One node
// can be wrong; the median of several cannot easily be, and repeaters are
// usually set from GPS or a phone. So: keep one sample per node, take the median
// of the recent ones, and when enough of them agree that we are off by more than
// a margin, step the clock by the median. A GPS fix outranks it - the location
// provider sets the clock itself - and every step is logged with the offset and
// the count, so a wrong mesh would be visible rather than believed.
//
// Pure, so the rule has tests. The caller owns the state and does the setting.

#define RIFT_CLOCK_SAMPLES     8
#define RIFT_CLOCK_MIN_AGREE   3        // nodes that must agree before a step
#define RIFT_CLOCK_AGREE_S     60       // within this many seconds of the median
#define RIFT_CLOCK_STEP_S      90       // an offset smaller than this is left alone
#define RIFT_CLOCK_SAMPLE_MS   1800000u // a sample older than 30 minutes is forgotten
// 1 January 2025. MeshCore's power-on default is 15 May 2024, so a sender whose
// clock reads before this is on that default, not on a time, and must not vote.
#define RIFT_CLOCK_PLAUSIBLE   1735689600u

struct RiftClockSample {
  uint8_t  key[6];
  int32_t  delta;      // their clock minus ours, seconds, at the moment of receipt
  uint32_t at_ms;      // when the sample was taken
  bool     used;
};

struct RiftClockSync {
  RiftClockSample s[RIFT_CLOCK_SAMPLES];
  int32_t  last_step;      // seconds the clock was last moved by, 0 if never
  uint32_t last_step_ms;
  uint16_t steps;          // how many times it has been moved since boot
};

static inline void riftClockReset(RiftClockSync* c) {
  if (c == NULL) return;
  memset(c, 0, sizeof(*c));
}

// One advert heard: their timestamp against our clock. A sender whose clock is
// plainly unset contributes nothing - it is not a measurement of anything.
// Returns false if the sample was refused.
static inline bool riftClockNote(RiftClockSync* c, const uint8_t* key6, uint32_t theirs,
                                 uint32_t ours, uint32_t now_ms) {
  if (c == NULL || key6 == NULL) return false;
  if (theirs < RIFT_CLOCK_PLAUSIBLE) return false;
  int32_t delta = (int32_t) (theirs - ours);

  // one slot per node: the same node advertising every minute must not be
  // counted as several nodes agreeing with itself. A new node takes a free
  // slot, or the stalest one.
  int slot = -1;
  for (int i = 0; i < RIFT_CLOCK_SAMPLES && slot < 0; i++) {
    if (c->s[i].used && memcmp(c->s[i].key, key6, 6) == 0) slot = i;
  }
  for (int i = 0; i < RIFT_CLOCK_SAMPLES && slot < 0; i++) {
    if (!c->s[i].used) slot = i;
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < RIFT_CLOCK_SAMPLES; i++) {
      if ((now_ms - c->s[i].at_ms) > (now_ms - c->s[slot].at_ms)) slot = i;
    }
  }
  memcpy(c->s[slot].key, key6, 6);
  c->s[slot].delta = delta;
  c->s[slot].at_ms = now_ms;
  c->s[slot].used = true;
  return true;
}

// The median offset of the samples still fresh, and how many of them sit within
// RIFT_CLOCK_AGREE_S of it. Returns the number of fresh samples (0 = no opinion).
static inline int riftClockConsensus(const RiftClockSync* c, uint32_t now_ms,
                                     int32_t* median, int* agreeing) {
  if (median) *median = 0;
  if (agreeing) *agreeing = 0;
  if (c == NULL) return 0;

  int32_t v[RIFT_CLOCK_SAMPLES];
  int n = 0;
  for (int i = 0; i < RIFT_CLOCK_SAMPLES; i++) {
    if (!c->s[i].used) continue;
    if ((now_ms - c->s[i].at_ms) > RIFT_CLOCK_SAMPLE_MS) continue;
    v[n++] = c->s[i].delta;
  }
  if (n == 0) return 0;

  // insertion sort: eight values at most
  for (int i = 1; i < n; i++) {
    int32_t x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  int32_t med = (n & 1) ? v[n / 2] : (int32_t) (((int64_t) v[n / 2 - 1] + v[n / 2]) / 2);

  int agree = 0;
  for (int i = 0; i < n; i++) {
    int32_t d = v[i] - med;
    if (d < 0) d = -d;
    if (d <= RIFT_CLOCK_AGREE_S) agree++;
  }
  if (median) *median = med;
  if (agreeing) *agreeing = agree;
  return n;
}

// Whether the consensus is worth acting on: enough nodes agree, and the offset
// is more than the margin. The margin exists so a clock a few seconds out is not
// nudged on every advert, and so that adverts delayed by the mesh (a flood can
// take seconds) do not read as a slow clock.
static inline bool riftClockShouldStep(int agreeing, int32_t median) {
  if (agreeing < RIFT_CLOCK_MIN_AGREE) return false;
  return median > RIFT_CLOCK_STEP_S || median < -RIFT_CLOCK_STEP_S;
}

// After the clock has been moved by `by` seconds every sample is that much
// closer to zero - they were measured against the old clock. Adjusted rather
// than cleared, so the readout can still say what the mesh thinks.
static inline void riftClockStepped(RiftClockSync* c, int32_t by, uint32_t now_ms) {
  if (c == NULL) return;
  for (int i = 0; i < RIFT_CLOCK_SAMPLES; i++) {
    if (c->s[i].used) c->s[i].delta -= by;
  }
  c->last_step = by;
  c->last_step_ms = now_ms;
  if (c->steps < 0xFFFF) c->steps++;
}

// Signed, compact: "+3m", "-42s", "+2h". For the CLOCK row.
static inline void riftClockOffsetText(int32_t secs, char* out, int out_sz) {
  if (out == NULL || out_sz < 2) return;
  const char* sign = secs < 0 ? "-" : "+";
  uint32_t a = (uint32_t) (secs < 0 ? -secs : secs);
  if (a < 60)        snprintf(out, (size_t) out_sz, "%s%us", sign, (unsigned) a);
  else if (a < 3600) snprintf(out, (size_t) out_sz, "%s%um", sign, (unsigned) (a / 60));
  else if (a < 86400) snprintf(out, (size_t) out_sz, "%s%uh", sign, (unsigned) (a / 3600));
  else               snprintf(out, (size_t) out_sz, "%s%ud", sign, (unsigned) (a / 86400));
}

// The one instance. Fed from MyMesh on every advert, read by SYSTEM.
inline RiftClockSync& riftClockState() {
  static RiftClockSync c = {};
  return c;
}
