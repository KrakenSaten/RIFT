#include "TDeckTouch.h"

// GT911 uses 16-bit register addresses
#define GT911_REG_STATUS   0x814E
#define GT911_REG_POINT1   0x8150

static bool gt911_read(uint16_t reg, uint8_t* buf, int len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;   // repeated start

  int got = Wire.requestFrom((int) TOUCH_I2C_ADDR, len);
  if (got != len) return false;
  for (int i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool gt911_write(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void TDeckTouch::begin() {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  _present = (Wire.endTransmission() == 0);
}

bool TDeckTouch::poll(int& x, int& y) {
  if (!_present) return false;

  unsigned long now = millis();
  if (now - _last_poll < TOUCH_POLL_MILLIS) return false;
  _last_poll = now;

  uint8_t status;
  if (!gt911_read(GT911_REG_STATUS, &status, 1)) return false;

  // bit 7 means the controller has fresh data for us; the low nibble is the
  // number of touch points it is reporting
  if ((status & 0x80) == 0) return false;
  int points = status & 0x0F;

  if (points > 0) {
    uint8_t d[8];
    if (gt911_read(GT911_REG_POINT1, d, sizeof(d))) {
      memcpy(_raw, d, sizeof(_raw));

      // The read starts at 0x8150, which the datasheet defines as point 1's X low
      // byte - the track id at 0x814F is simply not read. So X is at offset 0-1
      // and Y at 2-3 here, as the datasheet says for this start address. (An
      // earlier comment claimed the layout differed from the datasheet; it does
      // not, and the claim invited a "correction" that would have broken it.)
      _raw_x = d[0] | (d[1] << 8);
      _raw_y = d[2] | (d[3] << 8);

      // Portrait panel (240x320) to landscape display (320x240): raw Y runs along
      // display X and raw X runs against display Y, each scaled between the raw
      // values measured at the display's edges (TOUCH_RAW_* in the header) and
      // clamped. The swap and invert alone put raw (228, 8) at (8, 11) rather than
      // (0, 0), which is what the calibration points were recorded to prevent.
      {
        long rx = _raw_x, ry = _raw_y;
        long x = (ry - TOUCH_RAW_Y_AT_LEFT) * (TOUCH_RAW_H - 1)
                 / (TOUCH_RAW_Y_AT_RIGHT - TOUCH_RAW_Y_AT_LEFT);
        long y = (TOUCH_RAW_X_AT_TOP - rx) * (TOUCH_RAW_W - 1)
                 / (TOUCH_RAW_X_AT_TOP - TOUCH_RAW_X_AT_BOTTOM);
        if (x < 0) x = 0;
        if (x > TOUCH_RAW_H - 1) x = TOUCH_RAW_H - 1;
        if (y < 0) y = 0;
        if (y > TOUCH_RAW_W - 1) y = TOUCH_RAW_W - 1;
        _x = (int) x;
        _y = (int) y;
      }
      // only a successful read counts as a touch. Setting this regardless meant
      // a failed point read still produced a release event, carrying whatever
      // coordinates the previous touch had left behind.
      // Per-sample movement, measured before anything above the driver sees it.
      // A step far larger than a finger can travel in one interval is the panel
      // reporting a different contact, not a fast drag.
      if (_prev_y >= 0) {
        int step = _y - _prev_y;
        if (step < 0) step = -step;
        _drag_travel += step;
        if (step > _drag_max_step) _drag_max_step = step;
      }
      _prev_y = _y;
      _drag_samples++;

      _down = true;
    }
  }

  // the status register must be cleared or the controller stops reporting
  gt911_write(GT911_REG_STATUS, 0);

  if (points == 0 && _down) {   // finger lifted - this completes a tap
    _down = false;
    _prev_y = -1;   // the next contact starts its own measurement
    x = _x;
    y = _y;
    return true;
  }
  return false;
}
