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
    clearCounts();
    _last_activity = now;
  }

  // One gesture reports one direction, and firing clears the other three.
  //
  // Without that, a sustained roll walked off the screen it was scrolling. The ball
  // is a sphere against four mechanical switches, so rolling vertically also
  // pulses the side switches a little - unavoidably. Each counter was only cleared
  // by reaching its own threshold or by a full second of total idleness, and a
  // continuous roll is never idle, so that cross-axis trickle accumulated until it
  // crossed the threshold and reported KEY_LEFT or KEY_RIGHT. On a screen with a
  // list, that is a screen change in the middle of scrolling - and the faster the
  // roll, the sooner it happened, since the noise scales with the pulse count.
  //
  // Clearing the rest on each report bounds the trickle to whatever arrives inside
  // one step, which is far below the threshold at any realistic noise ratio.
  if (_count_up >= TRACKBALL_PULSE_THRESHOLD)    { clearCounts(); return KEY_UP; }
  if (_count_down >= TRACKBALL_PULSE_THRESHOLD)  { clearCounts(); return KEY_DOWN; }
  if (_count_left >= TRACKBALL_PULSE_THRESHOLD)  { clearCounts(); return KEY_LEFT; }
  if (_count_right >= TRACKBALL_PULSE_THRESHOLD) { clearCounts(); return KEY_RIGHT; }
  return 0;
}
