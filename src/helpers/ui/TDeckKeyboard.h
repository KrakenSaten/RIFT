#pragma once

#include <Arduino.h>
#include <Wire.h>

#ifndef KEYBOARD_I2C_ADDR
  #define KEYBOARD_I2C_ADDR 0x55
#endif
#ifndef KEYBOARD_I2C_SDA
  #define KEYBOARD_I2C_SDA 18
#endif
#ifndef KEYBOARD_I2C_SCL
  #define KEYBOARD_I2C_SCL 8
#endif
#ifndef KEYBOARD_POLL_MILLIS
  #define KEYBOARD_POLL_MILLIS 100
#endif
#ifndef KEYBOARD_BOOT_MILLIS
  #define KEYBOARD_BOOT_MILLIS 1500   // how long to keep probing for the co-processor
#endif
// How long the driver waits before probing a keyboard it has given up on. The point
// of giving up is to keep a broken peripheral off the main loop, which a probe every
// few seconds does not undo - and without it a transient I2C glitch cost the physical
// keyboard until the next reboot, in the field, with no way to tell why.
#ifndef KEYBOARD_REPROBE_MILLIS
  #define KEYBOARD_REPROBE_MILLIS 4000
#endif

#ifndef KEYBOARD_MAX_FAILURES
  #define KEYBOARD_MAX_FAILURES 10
#endif

// Reads the T-Deck's onboard I2C keyboard co-processor (an ESP32-C3 running
// LilyGO's keyboard firmware, a *software* I2C slave at 0x55).
//
// Deliberately follows the pattern proven by LilyGO's own examples and
// Meshtastic, because naive approaches wedge the board:
//   - the co-processor shares the GPIO10 peripheral power rail and needs
//     several hundred ms after power-on before it will ACK, so begin() probes
//     until it answers rather than reading blind;
//   - the "keyboard interrupt" line (GPIO46) is NOT driven by the stock
//     keyboard firmware, so it must not be used to gate reads;
//   - every read is guarded by a cheap address probe and the poll rate is
//     throttled, so an absent or wedged keyboard costs a bounded amount of
//     time instead of blocking the main loop on Wire timeouts every tick;
//   - after repeated failures the driver disables itself permanently.
//
// Reuses the Wire bus initialised elsewhere - does not call Wire.begin().
class TDeckKeyboard {
  bool _present;
  uint8_t _failures;
  bool _lost;                 // was present, then stopped answering
  unsigned long _last_reprobe;
  // elapsed-since, not deadline-in-future: millis() wraps after ~49 days and a
  // future deadline compares as already-past across the wrap
  unsigned long _last_poll;
  uint8_t _seen[8];      // I2C addresses that responded at begin(), for diagnostics
  uint8_t _seen_count;
  char _last_raw;        // previous raw read, for edge detection
  uint8_t _last_seen;    // last non-zero byte from the co-processor, incl. discarded ones

  uint8_t scanBus();

public:
  TDeckKeyboard() : _present(false), _failures(0), _lost(false), _last_reprobe(0),
                    _last_poll(0), _seen_count(0), _last_raw(0), _last_seen(0) { }

  void begin();

  bool isPresent() const { return _present; }
  // Present at boot and not answering now. SYSTEM can say "lost" rather than
  // "not found", which are different faults: one is a keyboard that was never
  // there, the other one that stopped.
  bool wasLost() const { return _lost; }

  // addresses seen on the bus at begin() - shown on the SYSTEM screen so the
  // real bus layout can be read off the device itself
  uint8_t seenCount() const { return _seen_count; }
  uint8_t seenAddr(uint8_t i) const { return _seen[i]; }

  // last byte the keyboard produced, whether or not we acted on it - lets key
  // codes be discovered from the device instead of assumed
  uint8_t lastSeen() const { return _last_seen; }

  // NOTE: there is deliberately no heldKey()/isHeld() here. This keyboard reports
  // one event per physical press and nothing at all while the key stays down -
  // measured, see poll(). A long press cannot be detected. Do not add an API that
  // implies otherwise.

  // returns the ASCII key value, or 0 if no key is available
  char poll();
};
