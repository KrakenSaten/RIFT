#pragma once

#include <Arduino.h>

// Short alert tones through the T-Deck's I2S amplifier.
//
// This board has no buzzer on a GPIO. It has a MAX98357A class-D amplifier driven
// over I2S, which is why the shared `genericBuzzer` cannot be used: that drives a
// square wave onto one pin, and the amplifier needs a bit clock and a frame clock
// as well. A raw waveform on the data pin produces silence or noise. So RIFT's
// notify() compiled to nothing on this variant until this existed.
//
// **The pin numbers are the one thing here that has not been verified against the
// hardware.** They are build flags rather than constants for that reason - see
// variants/lilygo_tdeck/platformio.ini. If there is no sound, check them first, and
// check `SOUND` on SYSTEM to tell "the driver never started" from "the driver ran
// and you heard nothing".
//
// Non-blocking, because there is no watchdog on the main loop and a blocking call
// silently starves the LoRa radio - the most important constraint in this codebase.
// loop() writes only as much as the DMA buffer will accept right now, with a zero
// wait, and returns. A tone therefore takes as many passes as it takes.
class TDeckSpeaker {
public:
  struct Step { uint16_t hz; uint16_t ms; };   // hz 0 = a rest

  TDeckSpeaker() { }

  // Returns false if the I2S driver would not install. Safe to call once; every
  // other method is a no-op until it succeeds.
  bool begin(int bclk, int lrclk, int dout, int sample_rate = 16000);

  bool isPresent() const { return _ok; }

  // Replaces whatever is playing. Alerts are short and the newest one is the one
  // that matters, so there is no queue to fall behind.
  //
  // gain is 0..100 percent of the driver's own amplitude. Discretion is partly
  // loudness and not only pitch: a low note at full scale is still an interruption,
  // and the difference between "you have a message" and "something is near you"
  // should be audible in how much it insists.
  void play(const Step* steps, int count, uint8_t gain = 100);

  bool isPlaying() const { return _ok && _step < _count; }
  void stop();

  // Call every main-loop pass. Does nothing when idle.
  void loop();

  // Frames handed to the driver since boot. On screen so "no sound" can be told
  // apart from "no code ran" without a logic analyser.
  uint32_t framesWritten() const { return _frames; }

private:
  static const int MAX_STEPS = 8;

  bool _ok = false;
  int  _rate = 16000;
  Step _seq[MAX_STEPS];
  int  _count = 0;
  int  _step = 0;
  uint32_t _step_started = 0;
  uint32_t _phase = 0;         // 16.16 fixed point, so no float in the inner loop
  uint32_t _frames = 0;
  uint8_t  _gain = 100;
};
