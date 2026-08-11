#pragma once

#include "DisplayDriver.h"
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <helpers/RefCountedDigitalPin.h>

// Native 320x240 driver for the T-Deck's ST7789 LCD (RIFT UI).
// Unlike ST7789LCDDisplay, this reports the true panel resolution instead of
// a scaled-up 128x64 OLED canvas, so RIFT screens draw at native coordinates.
class ST7789NativeDisplay : public DisplayDriver {
  SPIClass displaySPI;
  Adafruit_ST7789 display;
  bool _isOn;
  uint16_t _color;
  RefCountedDigitalPin* _peripher_power;

public:
  ST7789NativeDisplay(RefCountedDigitalPin* peripher_power=NULL) : DisplayDriver(320, 240),
      displaySPI(HSPI),
      display(&displaySPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST),
      _peripher_power(peripher_power)
  {
    _isOn = false;
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
