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
};
