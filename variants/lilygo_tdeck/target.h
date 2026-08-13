#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <TDeckBoard.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#ifdef DISPLAY_CLASS
  #ifdef RIFT_DISPLAY
    #include <helpers/ui/ST7789NativeDisplay.h>
  #else
    #include <helpers/ui/ST7789LCDDisplay.h>
  #endif
  #include <helpers/ui/MomentaryButton.h>
#endif
#ifdef RIFT_INPUT_KEYBOARD
  #include <helpers/ui/TDeckKeyboard.h>
#endif
#ifdef RIFT_INPUT_TRACKBALL
  #include <helpers/ui/TDeckTrackball.h>
#endif
#ifdef RIFT_INPUT_TOUCH
  #include <helpers/ui/TDeckTouch.h>
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"

extern TDeckBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif
#ifdef RIFT_INPUT_KEYBOARD
  extern TDeckKeyboard rift_keyboard;
#endif
#ifdef RIFT_INPUT_TRACKBALL
  extern TDeckTrackball rift_trackball;
#endif
#ifdef RIFT_INPUT_TOUCH
  extern TDeckTouch rift_touch;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
