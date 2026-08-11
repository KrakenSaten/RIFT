#include "ST7789NativeDisplay.h"

#ifndef PIN_TFT_MISO
  #define PIN_TFT_MISO -1
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3
#endif

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320

// RIFT color scheme: dark cyberdeck/radio-terminal palette.
ColorVal UIColor::window_bkg = ST77XX_BLACK;
ColorVal UIColor::title_bkg = (2 << 11) | (4 << 5) | 8;    // near-black navy
ColorVal UIColor::title_txt = ST77XX_CYAN;
ColorVal UIColor::primary_txt = ST77XX_GREEN;
ColorVal UIColor::secondary_txt = (8 << 11) | (18 << 5) | 10; // dim gray-green
ColorVal UIColor::warning_txt = ST77XX_ORANGE;
ColorVal UIColor::popup_bkg = (2 << 11) | (6 << 5) | 12;   // dark navy
ColorVal UIColor::popup_txt = ST77XX_CYAN;
ColorVal UIColor::corp_blue = ST77XX_ORANGE;   // RIFT accent (radar/signal elements)

bool ST7789NativeDisplay::begin() {
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();

    if (PIN_TFT_LEDA_CTL != -1) {
      pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
      digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    }

    displaySPI.begin(PIN_TFT_SCL, PIN_TFT_MISO, PIN_TFT_SDA, PIN_TFT_CS);

    display.init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    display.setRotation(DISPLAY_ROTATION);

    display.setSPISpeed(40e6);

    display.fillScreen(ST77XX_BLACK);
    display.setTextColor(ST77XX_GREEN);
    display.setTextSize(2);
    display.cp437(true); // Use full 256 char 'Code Page 437' font

    _isOn = true;
  }

  return true;
}

void ST7789NativeDisplay::turnOn() {
  ST7789NativeDisplay::begin();
}

void ST7789NativeDisplay::turnOff() {
  if (_isOn) {
    if (PIN_TFT_LEDA_CTL != -1) {
      digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    }
    if (PIN_TFT_RST != -1) {
      digitalWrite(PIN_TFT_RST, LOW);
    }
    if (PIN_TFT_LEDA_CTL != -1) {
      digitalWrite(PIN_TFT_LEDA_CTL, LOW);
    }
    _isOn = false;

    if (_peripher_power) _peripher_power->release();
  }
}

void ST7789NativeDisplay::clear() {
  display.fillScreen(ST77XX_BLACK);
}

void ST7789NativeDisplay::startFrame(ColorVal bkg) {
  display.fillScreen(bkg);
  display.setTextColor(_color = UIColor::primary_txt);
  display.setTextSize(1);
  display.cp437(true);
}

void ST7789NativeDisplay::setTextSize(int sz) {
  display.setTextSize(sz);
}

void ST7789NativeDisplay::setColor(ColorVal c) {
  display.setTextColor(_color = c);
}

void ST7789NativeDisplay::setCursor(int x, int y) {
  display.setCursor(x, y);
}

void ST7789NativeDisplay::print(const char* str) {
  display.print(str);
}

void ST7789NativeDisplay::fillRect(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, _color);
}

void ST7789NativeDisplay::drawRect(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, _color);
}

void ST7789NativeDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  uint8_t byteWidth = (w + 7) / 8;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[j * byteWidth + i / 8];
      bool pixelOn = byte & (0x80 >> (i & 7));

      if (pixelOn) {
        display.drawPixel(x + i, y + j, _color);
      }
    }
  }
}

uint16_t ST7789NativeDisplay::getTextWidth(const char* str) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);

  return w;
}

void ST7789NativeDisplay::endFrame() {
  // display.display();
}
