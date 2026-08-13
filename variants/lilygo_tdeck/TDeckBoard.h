#pragma once

#include <Wire.h>
#include <Arduino.h>
#include "helpers/ESP32Board.h"

#define PIN_VBAT_READ 4
#define BATTERY_SAMPLES 8
#define ADC_MULTIPLIER (2.0f * 3.3f * 1000)

class TDeckBoard : public ESP32Board {
public:
  void begin();

  #ifdef P_LORA_TX_LED
    void onBeforeTransmit() override{
      digitalWrite(P_LORA_TX_LED, LOW); // turn TX LED on - invert pin for SX1276
    }

    void onAfterTransmit() override{
      digitalWrite(P_LORA_TX_LED, HIGH); // turn TX LED off - invert pin for SX1276
    }
  #endif

  uint16_t getBattMilliVolts() {
    #if defined(PIN_VBAT_READ) && defined(ADC_MULTIPLIER)
      analogReadResolution(12);

      uint32_t raw = 0;
      for (int i = 0; i < BATTERY_SAMPLES; i++) {
        raw += analogRead(PIN_VBAT_READ);
      }

      raw = raw / BATTERY_SAMPLES;
      return (ADC_MULTIPLIER * raw) / 4096;
    #else
      return 0;
    #endif
  }

  // The base class returns false and no ESP32 board overrides it, so anything
  // depending on external power (KEEP_DISPLAY_ON_USB) was previously dead.
  //
  // Uses HWCDC::isPlugged(), which reads the USB-Serial-JTAG bus connection
  // status - i.e. whether a cable is attached. Deliberately NOT `(bool) Serial`:
  // that resolves to isCDC_Connected(), which additionally requires the host to
  // have *opened* the port, returns false on its first call by design, and so
  // stays false with a charger or an unopened port.
  bool isExternalPowered() override {
  #if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    return HWCDC::isPlugged();
  #else
    return false;
  #endif
  }

  const char* getManufacturerName() const{
    return "LilyGo T-Deck";
  }
};