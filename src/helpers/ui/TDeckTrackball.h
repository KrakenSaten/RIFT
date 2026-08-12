#pragma once

#include <Arduino.h>

#ifndef TRACKBALL_PULSE_THRESHOLD
  #define TRACKBALL_PULSE_THRESHOLD 3   // pulses needed before a nav step is reported
#endif
#ifndef TRACKBALL_DEBOUNCE_MILLIS
  #define TRACKBALL_DEBOUNCE_MILLIS 4
#endif
#ifndef TRACKBALL_IDLE_RESET_MILLIS
  #define TRACKBALL_IDLE_RESET_MILLIS 1000
#endif

// Reads the T-Deck trackball's four directional pulse switches (idle HIGH via
// pull-up, pulse LOW on movement) by plain polling from poll() - same pattern
// as this codebase's existing MomentaryButton, deliberately not
// interrupt-driven, to avoid any risk of an interrupt storm from a pin whose
// idle level doesn't match our pull-up assumption.
class TDeckTrackball {
  int8_t _up, _down, _left, _right;
  bool _prev_up, _prev_down, _prev_left, _prev_right;
  unsigned long _last_up, _last_down, _last_left, _last_right;
  unsigned long _last_activity;
  uint8_t _count_up, _count_down, _count_left, _count_right;

  bool pulse(int8_t pin, bool& prev, unsigned long& last);

public:
  TDeckTrackball(int8_t up, int8_t down, int8_t left, int8_t right)
    : _up(up), _down(down), _left(left), _right(right),
      _prev_up(true), _prev_down(true), _prev_left(true), _prev_right(true),
      _last_up(0), _last_down(0), _last_left(0), _last_right(0),
      _last_activity(0),
      _count_up(0), _count_down(0), _count_left(0), _count_right(0) { }

  void begin();

  // returns KEY_UP/KEY_DOWN/KEY_LEFT/KEY_RIGHT (from UIScreen.h) once enough
  // pulses have accumulated in one direction, or 0. Call frequently (every
  // main loop tick) so brief pulses aren't missed.
  char poll();
};
