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
  unsigned long _next_poll;
  uint8_t _seen[8];      // I2C addresses that responded at begin(), for diagnostics
  uint8_t _seen_count;

  uint8_t scanBus();

public:
  TDeckKeyboard() : _present(false), _failures(0), _next_poll(0), _seen_count(0) { }

  void begin();

  bool isPresent() const { return _present; }

  // addresses seen on the bus at begin() - shown on the SYSTEM screen so the
  // real bus layout can be read off the device itself
  uint8_t seenCount() const { return _seen_count; }
  uint8_t seenAddr(uint8_t i) const { return _seen[i]; }

  // returns the ASCII key value, or 0 if no key is available
  char poll();
};
