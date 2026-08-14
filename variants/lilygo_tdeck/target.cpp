#include <Arduino.h>
#include "target.h"

TDeckBoard board;

#if defined(P_LORA_SCLK)
  static SPIClass spi;
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif
#ifdef RIFT_INPUT_KEYBOARD
  TDeckKeyboard rift_keyboard;
#endif
#ifdef RIFT_INPUT_TRACKBALL
  TDeckTrackball rift_trackball(PIN_TRACKBALL_UP, PIN_TRACKBALL_DOWN, PIN_TRACKBALL_LEFT, PIN_TRACKBALL_RIGHT);
#endif
#ifdef RIFT_INPUT_TOUCH
  TDeckTouch rift_touch;
#endif

bool radio_init() {
  fallback_clock.begin();
  Wire.begin(18, 8);
  rtc_clock.begin(Wire);

  // Measured on hardware: after the RTC auto-discovery probe, a transaction to
  // an address nothing answers on takes ~920ms instead of the microseconds a
  // NACK should cost. EnvironmentSensorManager::begin() and TDeckKeyboard::begin()
  // each walk all 112 addresses, which was about four minutes of boot before the
  // mesh screen appeared.
  //
  // Ending and restarting the bus restores normal timing. TDeckKeyboard already
  // did exactly this to find its co-processor at all - doing it here instead
  // means every later user of the bus gets a working one, not just the keyboard.
  // Why the probe leaves the peripheral in that state is not established; this
  // treats the symptom, and the boot-phase timings on SYSTEM show whether it
  // still works.
  Wire.end();
  Wire.begin(18, 8, 100000UL);
  Wire.setTimeOut(50);   // bound any transaction that still misbehaves

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}
