#include "TDeckKeyboard.h"

// cheap address probe - returns true if something ACKs at the given address
static bool i2c_probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

uint8_t TDeckKeyboard::scanBus() {
  _seen_count = 0;
  for (uint8_t addr = 1; addr < 127 && _seen_count < 8; addr++) {
    if (i2c_probe(addr)) _seen[_seen_count++] = addr;
  }
  return _seen_count;
}

void TDeckKeyboard::begin() {
  Wire.setTimeOut(50);   // bound each transaction rather than relying on the default

  // The bus is initialised elsewhere, but that happens after an RTC probe runs
  // against it - if it ended up unconfigured we'd silently see an empty bus, so
  // re-initialise on the known T-Deck pins and re-scan before giving up.
  if (scanBus() == 0) {
    Wire.end();
    Wire.begin(KEYBOARD_I2C_SDA, KEYBOARD_I2C_SCL, 100000UL);
    delay(50);
    scanBus();
  }

  // The co-processor boots off the shared peripheral power rail, which the
  // board brings up without waiting. Probe until it answers instead of
  // assuming a fixed delay - if it never does, stay disabled rather than
  // blocking the main loop on every subsequent read.
  unsigned long deadline = millis() + KEYBOARD_BOOT_MILLIS;
  while (millis() < deadline) {
    if (i2c_probe(KEYBOARD_I2C_ADDR)) {
      _present = true;
      break;
    }
    delay(50);
  }

  if (_present && _seen_count == 0) scanBus();   // refresh diagnostics
}

char TDeckKeyboard::poll() {
  if (!_present) return 0;

  unsigned long now = millis();
  if (now - _last_poll < KEYBOARD_POLL_MILLIS) return 0;
  _last_poll = now;

  if (!i2c_probe(KEYBOARD_I2C_ADDR)) {
    if (++_failures >= KEYBOARD_MAX_FAILURES) _present = false;  // give up for good
    return 0;
  }

  char key = 0;
  Wire.requestFrom((int) KEYBOARD_I2C_ADDR, 1);
  while (Wire.available() > 0) {
    key = (char) Wire.read();
    _failures = 0;
  }

  if (key != 0) _last_seen = (uint8_t) key;   // record before any filtering

  // stock firmware reports 0x00 when idle; anything >127 is bogus
  if ((uint8_t) key > 127) key = 0;

  // Edge detection: the co-processor keeps returning the same key while it is
  // held, so report it only on the transition out of idle. Without this a
  // single press fires repeatedly at our poll rate.
  char emit = (key != 0 && _last_raw == 0) ? key : 0;

  // The repeats the line above discards are the only evidence a key is being
  // held - this keyboard sends no key-up - so count them rather than throw them
  // away. Saturates instead of wrapping: a key wedged down for two hours must
  // not read as freshly pressed.
  if (key != 0 && key == _last_raw) {
    if (_held_polls < 0xFFFF) _held_polls++;
  } else {
    _held_polls = 0;
  }

  _last_raw = key;
  return emit;
}
