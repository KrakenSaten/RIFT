#pragma once

#include <Wire.h>
#include <Arduino.h>
#include "helpers/ESP32Board.h"

#define PIN_VBAT_READ 4
#define BATTERY_SAMPLES 8
// The divider between the cell and the ADC pin, and nothing else. It used to be
// (2.0f * 3.3f * 1000): the 2.0 is this, and the rest was an assumed full-scale
// reference that getBattMilliVolts() no longer needs - see there.
#define BATTERY_DIVIDER 2

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

  // Cell voltage in millivolts, from the chip's own calibration rather than from
  // an assumed reference.
  //
  // This used to scale the raw count: (2.0 * 3.3 * 1000 * raw) / 4096, which says
  // full scale is exactly 3.3V and the converter is linear. The ESP32-S3 is
  // neither. Its reference varies part to part, the curve bends near both rails,
  // and the factory measures each chip and writes the correction into eFuse.
  // analogReadMilliVolts() is the core's accessor for exactly that, and the
  // difference is typically 100-300mV.
  //
  // That error matters more than its size suggests. A lithium cell sits between
  // 3.84V and 3.73V for the middle thirty percent of its charge, so 110mV *is*
  // thirty points - an uncorrected reading does not shift the percentage, it
  // erases the meaning of it. See riftBattPercent().
  //
  // Instantaneous on purpose. Telemetry and the companion's battery command want a
  // measurement; the display wants a settled number, and smooths this itself,
  // because a reading taken during a 22dBm transmit is real and is not what a
  // battery gauge should show.
  uint16_t getBattMilliVolts() {
    #if defined(PIN_VBAT_READ)
      analogReadResolution(12);

      uint32_t mv = 0;
      for (int i = 0; i < BATTERY_SAMPLES; i++) {
        mv += analogReadMilliVolts(PIN_VBAT_READ);
      }

      return (uint16_t) ((mv / BATTERY_SAMPLES) * BATTERY_DIVIDER);
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