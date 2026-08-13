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

    if (_canvas == NULL) {
      // 150KB - malloc routes anything over 4KB to PSRAM on this board. If it
      // fails we fall back to drawing straight to the panel, which flickers but
      // still works.
      _canvas = new GFXcanvas16(width(), height());
      if (_canvas != NULL && _canvas->getBuffer() != NULL) {
        _target = _canvas;
        _canvas->cp437(true);
      } else {
        delete _canvas;
        _canvas = NULL;
        _target = &display;
      }
    }

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
  _target->fillScreen(bkg);          // in RAM when buffered, so not visible
  _target->setTextColor(_color = UIColor::primary_txt);
  _target->setTextSize(1);
  _target->cp437(true);
}

void ST7789NativeDisplay::setTextSize(int sz) {
  _textsize = sz < 1 ? 1 : sz;
  _target->setTextSize(sz);
}

// RIFT_GLYPH_OSLASH / _UC are placeholders inserted by the UTF-8 translation
// for characters the font cannot represent; see riftTranslateUTF8().
void ST7789NativeDisplay::drawSlashedO(bool upper) {
  int16_t x = _target->getCursorX();
  int16_t y = _target->getCursorY();
  int sz = _textsize;

  _target->write(upper ? 'O' : 'o');   // advances the cursor for us

  // stroke across the bowl: uppercase fills the cell, lowercase sits lower
  int top = upper ? 0 : 2 * sz;
  _target->drawLine(x, y + 7 * sz - 1, x + 5 * sz - 1, y + top, _color);
}

void ST7789NativeDisplay::setColor(ColorVal c) {
  _target->setTextColor(_color = c);
}

void ST7789NativeDisplay::setCursor(int x, int y) {
  _target->setCursor(x, y);
}

void ST7789NativeDisplay::print(const char* str) {
  // Written byte at a time so the two synthesized glyphs can be intercepted.
  // Everything else goes through the normal CP437 path.
  for (const char* p = str; *p; p++) {
    unsigned char c = (unsigned char) *p;
    if (c == RIFT_GLYPH_OSLASH || c == RIFT_GLYPH_OSLASH_UC) {
      drawSlashedO(c == RIFT_GLYPH_OSLASH_UC);
    } else {
      _target->write(c);
    }
  }
}

void ST7789NativeDisplay::fillRect(int x, int y, int w, int h) {
  _target->fillRect(x, y, w, h, _color);
}

void ST7789NativeDisplay::drawRect(int x, int y, int w, int h) {
  _target->drawRect(x, y, w, h, _color);
}

void ST7789NativeDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  uint8_t byteWidth = (w + 7) / 8;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[j * byteWidth + i / 8];
      bool pixelOn = byte & (0x80 >> (i & 7));

      if (pixelOn) {
        _target->drawPixel(x + i, y + j, _color);
      }
    }
  }
}

uint16_t ST7789NativeDisplay::getTextWidth(const char* str) {
  int16_t x1, y1;
  uint16_t w, h;
  _target->getTextBounds(str, 0, 0, &x1, &y1, &w, &h);

  return w;
}

void ST7789NativeDisplay::endFrame() {
  // Single bulk transfer of the finished frame. drawRGBBitmap goes through
  // writePixels(), so this is one SPI burst rather than per-pixel traffic.
  if (_canvas != NULL) {
    display.drawRGBBitmap(0, 0, _canvas->getBuffer(), width(), height());
  }
}
