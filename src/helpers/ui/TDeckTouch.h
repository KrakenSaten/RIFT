#pragma once

#include <Arduino.h>
#include <Wire.h>

// The GT911 answers on one of two addresses, and which one is not a build-time
// fact: it latches the address at power-on from the level on its INT pin, so two
// T-Deck units off different production runs can differ. This was set to 0x14 by a
// build flag and probed at that address only, so on a unit that came up at 0x5D
// touch reported "not found" and the whole drag and scroll surface was dead -
// while an I2C scan on the same device listed 0x5D plainly. Nothing was wrong with
// the panel or its connector.
//
// So both are tried, in this order, and the one that answers is used. The flag
// still sets which is preferred, for a board where that is known.
#ifndef TOUCH_I2C_ADDR
  #define TOUCH_I2C_ADDR 0x14
#endif
#ifndef TOUCH_I2C_ADDR_ALT
  #define TOUCH_I2C_ADDR_ALT 0x5D
#endif
// 25 ms was chosen when touch only had to notice a tap. Following a finger is a
// different job: at 40 samples a second a moderate drag advances ten pixels
// between samples, and the scroll can only move in those steps.
//
// One I2C transaction of a few bytes at 100 kHz is well under a millisecond, so
// the cost of the faster rate is negligible against a frame that spends tens of
// milliseconds pushing 153 KB over HSPI.
#ifndef TOUCH_POLL_MILLIS
  #define TOUCH_POLL_MILLIS 8
#endif
// GT911 reports in the panel's native portrait orientation. The display runs
// rotated to landscape, so raw coordinates are remapped - see poll().
#ifndef TOUCH_RAW_W
  #define TOUCH_RAW_W 240
#endif
#ifndef TOUCH_RAW_H
  #define TOUCH_RAW_H 320
#endif
// Calibration: the raw reading at each display edge, measured on hardware by
// touching the corners and reading the raw pair off SYSTEM. Display top-left
// gave raw (228, 8) and bottom-right raw (6, 310). Raw Y runs along display X
// and raw X runs against display Y. Build flags, so a panel that reads
// differently can be corrected without touching the driver.
//
// These were recorded in the driver's comment from the start and never applied:
// the mapping did only the axis swap and the invert, so a tap in the true corner
// arrived up to 11 pixels short of it, and the top rows of the nav bar could be
// missed by a finger that was on them.
#ifndef TOUCH_RAW_Y_AT_LEFT
  #define TOUCH_RAW_Y_AT_LEFT    8
#endif
#ifndef TOUCH_RAW_Y_AT_RIGHT
  #define TOUCH_RAW_Y_AT_RIGHT 310
#endif
#ifndef TOUCH_RAW_X_AT_TOP
  #define TOUCH_RAW_X_AT_TOP   228
#endif
#ifndef TOUCH_RAW_X_AT_BOTTOM
  #define TOUCH_RAW_X_AT_BOTTOM  6
#endif

// GT911 capacitive touch on the T-Deck.
//
// Polled rather than gated on the interrupt pin (GPIO16): the keyboard
// co-processor taught us that an INT line documented as available is not
// necessarily driven, and gating reads on it produces a silently dead input.
// Reuses the Wire bus initialised elsewhere.
class TDeckTouch {
  bool _present;
  uint8_t _addr;       // whichever of the two answered begin()
  unsigned long _last_poll;   // see TDeckKeyboard re: millis() wrap
  bool _down;
  int _x, _y;          // mapped to display coordinates
  int _raw_x, _raw_y;  // as reported, for calibration/diagnostics

public:
  TDeckTouch() : _present(false), _addr(TOUCH_I2C_ADDR), _last_poll(0), _down(false),
                 _x(0), _y(0), _raw_x(0), _raw_y(0) { memset(_raw, 0, sizeof(_raw)); }

  void begin();
  bool isPresent() const { return _present; }

  // Which address answered, for the diagnostics row. Worth showing rather than
  // assuming: it is the difference between a panel that is absent and one that is
  // simply not where it was looked for.
  uint8_t address() const { return _addr; }

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

  // Drag diagnostics, for the one question the code cannot answer by inspection:
  // is a jump on release panel noise, a clamp firing, or a sample rate too coarse
  // to follow the finger. Each has a different fix and they look identical from
  // the outside.
  //
  // Recorded here rather than in the UI because this is where the samples are.
  uint16_t dragSamples() const { return _drag_samples; }
  int      dragTravel() const { return _drag_travel; }   // sum of |dy| seen
  int      dragMaxStep() const { return _drag_max_step; }
  void     dragReset() { _drag_samples = 0; _drag_travel = 0; _drag_max_step = 0; }

private:
  uint16_t _drag_samples = 0;
  int _drag_travel = 0;
  int _drag_max_step = 0;
  int _prev_y = -1;
};
