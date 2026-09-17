#pragma once

#include "DisplayDriver.h"
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <helpers/RefCountedDigitalPin.h>

// Placeholders for the two characters the font cannot represent. Control-range
// values, so they can never collide with translated text.
#define RIFT_GLYPH_OSLASH     0x01   // o with stroke
#define RIFT_GLYPH_OSLASH_UC  0x02   // O with stroke

// Native 320x240 driver for the T-Deck's ST7789 LCD (RIFT UI).
// Unlike ST7789LCDDisplay, this reports the true panel resolution instead of
// a scaled-up 128x64 OLED canvas, so RIFT screens draw at native coordinates.
class ST7789NativeDisplay : public DisplayDriver {
  SPIClass displaySPI;
  Adafruit_ST7789 display;

  // Off-screen buffer. Every draw call goes here and the panel is written once
  // in endFrame(), so no intermediate state is ever visible - startFrame()
  // clearing the screen used to show as a black flash on every repaint.
  //
  // Costs 320*240*2 = 150KB, which lands in PSRAM (allocations over 4KB do).
  // Total SPI traffic actually drops: the old path wrote a full frame of
  // background and then overwrote much of it with content.
  GFXcanvas16* _canvas;
  Adafruit_GFX* _target;   // _canvas when buffered, &display as fallback

  bool _isOn;
  uint16_t _color;
  int _textsize;

  // What endFrame() costs, because the display work has to start from a measurement
  // and there has not been one.
  //
  // 320*240*2 = 153,600 bytes at the 40MHz set in begin() is 30.7ms of SPI clock,
  // and that is a floor rather than the figure: the canvas is in PSRAM, and
  // drawRGBBitmap reads it out through writePixels() before any of it reaches the
  // bus. Whether the read or the transfer dominates is not worth reasoning about
  // from the datasheet when it can be counted.
  //
  // It is also not merely time the radio is not being served. PIN_TFT_SCL is 40 and
  // PIN_TFT_SDA is 41, which are P_LORA_SCLK and P_LORA_MOSI - the panel and the
  // SX1262 sit on the same two wires behind different chip selects, so a blit is
  // time the radio cannot be reached at all.
  //
  // The maximum is the number that matters. A mean hides the single frame that ran
  // long, and the single frame that ran long is the one that cost a packet.
  uint32_t _blit_last_us;
  uint32_t _blit_max_us;
  uint64_t _blit_total_us;
  uint32_t _blit_count;

  // o/O with a stroke, drawn rather than looked up: CP437 (and so the Adafruit
  // classic font) has no slashed O at all, and the nearest glyph in the font is
  // a phi, which has ascenders and descenders that read badly as a letter.
  void drawSlashedO(bool upper);
  RefCountedDigitalPin* _peripher_power;

public:
  ST7789NativeDisplay(RefCountedDigitalPin* peripher_power=NULL) : DisplayDriver(320, 240),
      displaySPI(HSPI),
      display(&displaySPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST),
      _peripher_power(peripher_power)
  {
    _isOn = false;
    _textsize = 1;
    _canvas = NULL;
    _target = &display;
    _blit_last_us = 0;
    _blit_max_us = 0;
    _blit_total_us = 0;
    _blit_count = 0;
  }

  bool begin();

  bool isOn() override { return _isOn; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;

  // The composed frame, 320*240 RGB565 in native (little-endian) order, or NULL
  // when the driver fell back to drawing straight to the panel. Read by the
  // screen-dump command so a design round can work from the device's own
  // pixels rather than from photographs of it.
  const uint16_t* frameBuffer() const { return _canvas ? _canvas->getBuffer() : NULL; }

  // Blit cost, in microseconds. Zero count means nothing has been transferred -
  // either nothing has drawn yet, or the canvas allocation failed and there is no
  // bulk transfer to time. SYSTEM reads these; see the note beside the members.
  uint32_t blitLastMicros() const { return _blit_last_us; }
  uint32_t blitMaxMicros() const { return _blit_max_us; }
  uint32_t blitCount() const { return _blit_count; }
  uint32_t blitMeanMicros() const {
    return _blit_count ? (uint32_t) (_blit_total_us / _blit_count) : 0;
  }
  // So a screen can be watched on its own rather than through the average since
  // boot, which a few seconds on RADAR would otherwise dominate for the rest of
  // the session.
  void blitStatsReset() {
    _blit_last_us = 0; _blit_max_us = 0; _blit_total_us = 0; _blit_count = 0;
  }
};
