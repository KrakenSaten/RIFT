#pragma once

#include <Arduino.h>
#include <Wire.h>

#ifndef TOUCH_I2C_ADDR
  #define TOUCH_I2C_ADDR 0x14   // GT911; the alternate address is 0x5D
#endif
#ifndef TOUCH_POLL_MILLIS
  #define TOUCH_POLL_MILLIS 25
#endif
// GT911 reports in the panel's native portrait orientation. The display runs
// rotated to landscape, so raw coordinates are remapped - see poll().
#ifndef TOUCH_RAW_W
  #define TOUCH_RAW_W 240
#endif
#ifndef TOUCH_RAW_H
  #define TOUCH_RAW_H 320
#endif

// GT911 capacitive touch on the T-Deck.
//
// Polled rather than gated on the interrupt pin (GPIO16): the keyboard
// co-processor taught us that an INT line documented as available is not
// necessarily driven, and gating reads on it produces a silently dead input.
// Reuses the Wire bus initialised elsewhere.
class TDeckTouch {
  bool _present;
  unsigned long _next_poll;
  bool _down;
  int _x, _y;          // mapped to display coordinates
  int _raw_x, _raw_y;  // as reported, for calibration/diagnostics

public:
  TDeckTouch() : _present(false), _next_poll(0), _down(false),
                 _x(0), _y(0), _raw_x(0), _raw_y(0) { memset(_raw, 0, sizeof(_raw)); }

  void begin();
  bool isPresent() const { return _present; }

  // Returns true once per completed tap, with the release position in x/y.
  // Reporting on release rather than press avoids firing while a finger drags.
  bool poll(int& x, int& y);

  bool isDown() const { return _down; }
  int lastX() const { return _x; }
  int lastY() const { return _y; }
  int rawX() const { return _raw_x; }
  int rawY() const { return _raw_y; }

  // raw point-data bytes from the last touch, so the byte layout can be read
  // off the device instead of guessed at
  const uint8_t* rawBytes() const { return _raw; }
  uint8_t _raw[8];
};
