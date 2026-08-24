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
  unsigned long now = millis();

  // Given up on, but not for good. Polling stops at the normal rate so a broken
  // peripheral cannot hold up the main loop - there is no watchdog on it - and a
  // probe every four seconds is cheap enough not to reintroduce that while still
  // letting the keyboard come back. A transient I2C glitch used to cost the physical
  // keyboard until the next reboot.
  if (!_present) {
    if (!_lost) return 0;                        // never detected: nothing to recover
    if (now - _last_reprobe < KEYBOARD_REPROBE_MILLIS) return 0;
    _last_reprobe = now;
    if (i2c_probe(KEYBOARD_I2C_ADDR)) {
      _present = true;
      _lost = false;
      _failures = 0;
    }
    return 0;
  }

  if (now - _last_poll < KEYBOARD_POLL_MILLIS) return 0;
  _last_poll = now;

  if (!i2c_probe(KEYBOARD_I2C_ADDR)) {
    if (++_failures >= KEYBOARD_MAX_FAILURES) {
      _present = false;
      _lost = true;                              // eligible for the slow re-probe
      _last_reprobe = now;
    }
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

  // Edge detection, reporting a key only on the transition out of idle.
  //
  // The comment here used to say the co-processor repeats a held key and that
  // without this a single press would fire repeatedly at the poll rate. Measured
  // on hardware, that is not true of this unit: holding a key produces exactly one
  // event, and reads after it return 0 for as long as it stays down. Holding `a`
  // in a text field types one `a` and nothing more, however long you hold.
  //
  // So this suppresses nothing today. It is kept because it costs one comparison
  // and because a different keyboard firmware may well behave as the original
  // comment described - but do not build on it. There is no key repeat and no
  // key-up, which means **a long press cannot be detected on this hardware at
  // all**; anything needing a second gesture has to use discrete presses, which is
  // why the Nordic picker is on a double tap.
  char emit = (key != 0 && _last_raw == 0) ? key : 0;
  _last_raw = key;
  return emit;
}
