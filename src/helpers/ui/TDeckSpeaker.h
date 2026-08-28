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
// wait, and returns.
//
// The DMA queue is sized to hold a whole alert, so in practice a tone is handed
// over in one or two passes and then plays out on its own. That is the point: the
// main loop does a full-frame redraw and a filesystem write on the pass where a
// message arrives, which is exactly when the alert sounds, and a queue shorter
// than that pass is a queue that runs dry mid-tone.
class TDeckSpeaker {
public:
  struct Step { uint16_t hz; uint16_t ms; };   // hz 0 = a rest

  TDeckSpeaker() { }

  // Returns false if the I2S driver would not install. Safe to call once; every
  // other method is a no-op until it succeeds.
  bool begin(int bclk, int lrclk, int dout, int sample_rate = 16000);

  bool isPresent() const { return _ok; }

  // Starts a sequence, or holds it until the current one has finished sounding.
  //
  // It used to replace whatever was playing. With two messages arriving together
  // that cut one note part way through and restarted it, which is heard as one
  // broken beep rather than as two messages. The queue is one slot deep, so a
  // burst does not turn into a minute of beeping - the second alert is what says
  // it was more than one, and a third adds nothing.
  //
  // gain is 0..100 percent of the driver's own amplitude. Discretion is partly
  // loudness and not only pitch: a low note at full scale is still an interruption,
  // and the difference between "you have a message" and "something is near you"
  // should be audible in how much it insists.
  void play(const Step* steps, int count, uint8_t gain = 100);

  // True while a tone is still audible, which includes the stretch after the last
  // sample has been queued but before the DMA engine has played it. Reporting
  // "not playing" at the handover was how the tail came to be discarded.
  bool isPlaying() const { return _ok && (_step < _count || isDraining()); }
  bool isDraining() const;
  void stop();

  // Call every main-loop pass. Does nothing when idle.
  void loop();

  // Frames handed to the driver since boot. On screen so "no sound" can be told
  // apart from "no code ran" without a logic analyser.
  uint32_t framesWritten() const { return _frames; }

  // Underruns since boot: the number of times the DMA engine had consumed every
  // sample it was given while a tone was still in progress. That is the stutter,
  // stated directly rather than inferred.
  //
  // The first attempt at this measured the gap between loop() calls, which was
  // useless the moment the queue got big enough to take a whole tone in one pass:
  // there were no further passes, so the gap read 0 and would have read 0 however
  // badly the audio behaved. This compares the samples the engine must have
  // played by now against the samples handed over, which is the actual question.
  uint32_t underruns() const { return _underruns; }
  uint32_t bufferedMs() const;

  // The most passes any single tone needed to be written. One means the whole
  // tone went to the DMA engine in a single call, which is the healthy state and
  // the only one that has sounded right.
  //
  // This is the number that has actually tracked the fault: 1153, 20, 2 and 1
  // passes on four consecutive alerts, sounding bad, bad, good, good. The two
  // counters beside it could not fail; this one moved with what was heard.
  uint32_t maxPasses() const { return _max_passes; }

  // One line per tone, for the event log: how many samples it wrote, how many
  // passes that took, and how long the DMA queue had been empty before it
  // started. Returns false when there is nothing new.
  //
  // The two summary counters this sits next to have both turned out to be unable
  // to fail. The gap between loop() passes reads zero once a whole tone fits in
  // one pass, and the underrun count only ever runs while the generator is
  // working, which with a 320 ms queue is a single pass. A timeline can be wrong,
  // which is what makes it worth reading.
  bool takeEvent(char* out, int sz);

private:
  static const int MAX_STEPS = 8;

  bool _ok = false;
  int  _rate = 16000;
  Step _seq[MAX_STEPS];
  int  _count = 0;
  int  _step = 0;
  // Step position is counted in samples written, not in elapsed milliseconds. A
  // wall clock says how long ago the note started; only the sample count says how
  // much of it has actually been played. Timing it by the clock truncated every note
  // to whatever the loop managed to queue in that many milliseconds.
  uint32_t _step_samples = 0;   // length of the current step
  uint32_t _step_done = 0;      // of it, written
  uint32_t _phase = 0;          // 8.24 fixed point; the top byte indexes the table
  uint32_t _inc = 0;            // phase step per sample for the current note
  uint32_t _frames = 0;
  // Millis at which the first sample of this sequence reached the driver, and how
  // long the whole sequence lasts. Together they say when it has been heard.
  uint32_t _audio_started_at = 0;
  uint32_t _total_ms = 0;
  uint32_t _written_total = 0;   // samples handed over for this sequence
  uint32_t _underruns = 0;
  Step _pending[MAX_STEPS];
  int  _pending_count = 0;
  uint8_t _pending_gain = 100;
  bool _have_pending = false;
  // Whether the I2S engine is clocked. Halted between tones so every one starts
  // from the head of the descriptor ring rather than wherever the last one left
  // the write pointer.
  bool _running = false;
  // The driver hands back a ring already full of descriptors, so the first tone
  // has to write in lockstep with playback while it drains. Cleared once at boot
  // instead, long before anything needs to sound. See begin().
  uint32_t _prime_until = 0;
  // Filled when a tone finishes generating, consumed by the UI into the log.
  uint32_t _ev_samples = 0;
  uint32_t _ev_passes = 0;
  uint32_t _ev_silence_ms = 0;   // queue empty for this long before the tone
  bool _ev_ready = false;
  uint32_t _passes = 0;          // passes used by the tone being generated
  uint32_t _max_passes = 0;      // worst seen since boot
  uint32_t _prev_end_ms = 0;     // when the previous tone finished sounding
  uint8_t  _gain = 100;

  void beginStep();
};
