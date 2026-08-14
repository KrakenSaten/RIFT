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
  // elapsed-since, not deadline-in-future: millis() wraps after ~49 days and a
  // future deadline compares as already-past across the wrap
  unsigned long _last_poll;
  uint8_t _seen[8];      // I2C addresses that responded at begin(), for diagnostics
  uint8_t _seen_count;
  char _last_raw;        // previous raw read, for edge detection
  uint8_t _last_seen;    // last non-zero byte from the co-processor, incl. discarded ones
  uint16_t _held_polls;  // consecutive polls returning the same key - see heldPolls()

  uint8_t scanBus();

public:
  TDeckKeyboard() : _present(false), _failures(0), _last_poll(0), _seen_count(0), _last_raw(0), _last_seen(0),
                    _held_polls(0) { }

  void begin();

  bool isPresent() const { return _present; }

  // addresses seen on the bus at begin() - shown on the SYSTEM screen so the
  // real bus layout can be read off the device itself
  uint8_t seenCount() const { return _seen_count; }
  uint8_t seenAddr(uint8_t i) const { return _seen[i]; }

  // last byte the keyboard produced, whether or not we acted on it - lets key
  // codes be discovered from the device instead of assumed
  uint8_t lastSeen() const { return _last_seen; }

  // Which key is currently down, or 0. This is the co-processor's key repeat -
  // it keeps returning the same byte while a key is held - which poll() otherwise
  // only uses to suppress duplicates. Without exposing it there is no way at all
  // to detect a long press on a keyboard key: the co-processor sends no key-up.
  char heldKey() const { return _last_raw; }

  // How many polls the current key has been held *beyond* the initial press, so 0
  // means "just pressed". Multiply by KEYBOARD_POLL_MILLIS for a rough duration;
  // it is a count rather than a timestamp because the poll rate is what actually
  // bounds the resolution.
  uint16_t heldPolls() const { return _held_polls; }

  // returns the ASCII key value, or 0 if no key is available
  char poll();
};
