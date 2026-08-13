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
  if (now < _next_poll) return false;
  _next_poll = now + TOUCH_POLL_MILLIS;

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
    }
    _down = true;
  }

  // the status register must be cleared or the controller stops reporting
  gt911_write(GT911_REG_STATUS, 0);

  if (points == 0 && _down) {   // finger lifted - this completes a tap
    _down = false;
    x = _x;
    y = _y;
    return true;
  }
  return false;
}
