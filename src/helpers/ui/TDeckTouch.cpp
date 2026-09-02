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

      // Byte layout determined from the device rather than the datasheet: the
      // buffer this read returns has X at offset 0-1 and Y at 2-3, not the
      // documented track-id-first layout. Measured on hardware:
      //   display top-left     -> raw (228, 8)
      //   display bottom-right -> raw (6, 310)
      _raw_x = d[0] | (d[1] << 8);
      _raw_y = d[2] | (d[3] << 8);

      // Portrait panel (240x320) to landscape display (320x240): raw Y becomes
      // display X, and raw X is inverted to become display Y. Confirmed by the
      // two calibration points above.
      _x = _raw_y;
      _y = (TOUCH_RAW_W - 1) - _raw_x;
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
