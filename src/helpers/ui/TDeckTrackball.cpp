#include "TDeckTrackball.h"
#include "UIScreen.h"

void TDeckTrackball::begin() {
  pinMode(_up, INPUT_PULLUP);
  pinMode(_down, INPUT_PULLUP);
  pinMode(_left, INPUT_PULLUP);
  pinMode(_right, INPUT_PULLUP);
}

bool TDeckTrackball::pulse(int8_t pin, bool& prev, unsigned long& last) {
  bool level = digitalRead(pin);
  bool falling = prev && !level;   // HIGH -> LOW transition
  prev = level;

  if (!falling) return false;

  unsigned long now = millis();
  if (now - last < TRACKBALL_DEBOUNCE_MILLIS) return false;
  last = now;
  return true;
}

char TDeckTrackball::poll() {
  bool any = false;
  if (pulse(_up, _prev_up, _last_up)) { _count_up++; any = true; }
  if (pulse(_down, _prev_down, _last_down)) { _count_down++; any = true; }
  if (pulse(_left, _prev_left, _last_left)) { _count_left++; any = true; }
  if (pulse(_right, _prev_right, _last_right)) { _count_right++; any = true; }

  // drop stale partial counts so slow stray pulses don't accumulate into a
  // spurious nav step (same idea as Meshtastic's 1s idle reset)
  unsigned long now = millis();
  if (any) {
    _last_activity = now;
  } else if (now - _last_activity > TRACKBALL_IDLE_RESET_MILLIS) {
    _count_up = _count_down = _count_left = _count_right = 0;
    _last_activity = now;
  }

  if (_count_up >= TRACKBALL_PULSE_THRESHOLD) { _count_up = 0; return KEY_UP; }
  if (_count_down >= TRACKBALL_PULSE_THRESHOLD) { _count_down = 0; return KEY_DOWN; }
  if (_count_left >= TRACKBALL_PULSE_THRESHOLD) { _count_left = 0; return KEY_LEFT; }
  if (_count_right >= TRACKBALL_PULSE_THRESHOLD) { _count_right = 0; return KEY_RIGHT; }
  return 0;
}
