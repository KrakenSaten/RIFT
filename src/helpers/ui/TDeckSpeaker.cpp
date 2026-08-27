#include "TDeckSpeaker.h"

#if defined(ESP32)
#include <driver/i2s.h>
#include <math.h>

// Port 0. Nothing else in this firmware uses I2S, and the mesh radio is on SPI, so
// there is no contention to arbitrate.
#define SPK_PORT   I2S_NUM_0

// Sized to hold a whole alert, which is what makes the tone immune to the main
// loop. 10 x 512 samples is 5120 samples, 320 ms at 16 kHz, against a longest
// alert of 250 ms - so an entire tone is handed to the DMA engine in one or two
// passes and plays out at the sample rate whatever the loop does afterwards.
//
// It was 4 x 128 = 32 ms, and 32 ms is less than one main-loop iteration when a
// message arrives: that pass does a full-frame redraw, 153 KB over HSPI, plus the
// message-log write. The queue ran dry mid-tone and tx_desc_auto_clear put
// silence out, which is the stutter.
//
// A long queue was avoided before on the grounds that a tone would keep sounding
// after stop(). It does not: stop() zeroes the buffers.
#define SPK_DMA_COUNT  10
#define SPK_DMA_LEN    512

// A quarter of full scale. This amplifier is loud enough to be unpleasant at full
// amplitude in a room, and an alert has to be noticed rather than resented.
#define SPK_AMPLITUDE  8000

// Fade length at each end of a note, in samples. 64 at 16 kHz is 4 ms.
#define SPK_RAMP       64

// A 256-point sine, built once at begin(). Sixteen points was audibly rough: at 523
// Hz and 16 kHz each entry was held for two samples, so the output was a staircase
// rather than a tone, and the harmonics of that staircase are what "scratchy" was.
// 512 bytes of RAM for a clean tone is a good trade.
static int16_t s_sine[256];
static bool s_sine_ready = false;

bool TDeckSpeaker::begin(int bclk, int lrclk, int dout, int sample_rate) {
  if (_ok) return true;
  _rate = sample_rate > 0 ? sample_rate : 16000;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t) (I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = _rate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  // The amplifier is mono and takes the left channel. Asking for stereo would
  // double the frames written for no audible difference.
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = SPK_DMA_COUNT;
  cfg.dma_buf_len = SPK_DMA_LEN;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;   // silence rather than the last buffer on underrun

  if (i2s_driver_install(SPK_PORT, &cfg, 0, NULL) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = bclk;
  pins.ws_io_num = lrclk;
  pins.data_out_num = dout;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(SPK_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(SPK_PORT);
    return false;
  }

  i2s_zero_dma_buffer(SPK_PORT);

  if (!s_sine_ready) {
    for (int i = 0; i < 256; i++) {
      s_sine[i] = (int16_t) lrintf(32767.0f * sinf(2.0f * (float) M_PI * (float) i / 256.0f));
    }
    s_sine_ready = true;
  }

  _ok = true;
  return true;
}

// Sets up the current step: how many samples it lasts, and the phase increment for
// its pitch. Called on play() and on every step boundary.
void TDeckSpeaker::beginStep() {
  _step_done = 0;
  _phase = 0;
  if (_step >= _count) { _step_samples = 0; _inc = 0; return; }
  _step_samples = (uint32_t) _seq[_step].ms * (uint32_t) _rate / 1000u;
  const uint16_t hz = _seq[_step].hz;
  // 8.24: the top byte indexes 256 table entries, so inc is hz/rate of a full turn
  _inc = hz == 0 ? 0 : (uint32_t) (((uint64_t) hz << 32) / (uint32_t) _rate);
}

void TDeckSpeaker::play(const Step* steps, int count, uint8_t gain) {
  if (!_ok || steps == NULL || count <= 0) return;
  if (count > MAX_STEPS) count = MAX_STEPS;

  // A second alert while one is sounding waits for it, rather than replacing it.
  //
  // Replacing was the documented behaviour - "the newest one is the one that
  // matters" - and with two messages arriving together it meant one note was cut
  // part way through and restarted, which is heard as a single broken beep rather
  // than as two messages. One slot deep: a burst of ten should not queue ten
  // alerts, and the second one is what tells you it was more than one.
  if (isPlaying()) {
    if (!_have_pending) {
      _have_pending = true;
      _pending_count = count;
      _pending_gain = gain > 100 ? 100 : gain;
      for (int i = 0; i < count; i++) _pending[i] = steps[i];
    }
    return;
  }

  _gain = gain > 100 ? 100 : gain;
  for (int i = 0; i < count; i++) _seq[i] = steps[i];
  _count = count;
  _step = 0;
  _audio_started_at = 0;
  _written_total = 0;
  _total_ms = 0;
  _passes = 0;
  for (int i = 0; i < count; i++) _total_ms += _seq[i].ms;
  beginStep();
}

void TDeckSpeaker::stop() {
  if (!_ok) return;
  _count = _step = 0;
  _audio_started_at = 0;
  _written_total = 0;
  _total_ms = 0;
  _have_pending = false;
  i2s_zero_dma_buffer(SPK_PORT);   // an explicit cut, unlike finishing normally
}

// True until the audio has been heard, not until the samples have been handed
// over. The DMA engine consumes at exactly the sample rate, so a sequence is done
// _total_ms after its first sample went in; the margin covers the startup latency
// of one buffer.
bool TDeckSpeaker::isDraining() const {
  if (_audio_started_at == 0) return false;
  return (int32_t) (millis() - (_audio_started_at + _total_ms + 40)) < 0;
}

bool TDeckSpeaker::takeEvent(char* out, int sz) {
  if (!_ev_ready || out == NULL || sz <= 0) return false;
  _ev_ready = false;
  snprintf(out, (size_t) sz, "snd %usmp %upass sil %ums",
           (unsigned) _ev_samples, (unsigned) _ev_passes, (unsigned) _ev_silence_ms);
  return true;
}

uint32_t TDeckSpeaker::bufferedMs() const {
  return (uint32_t) SPK_DMA_COUNT * (uint32_t) SPK_DMA_LEN * 1000u / (uint32_t) _rate;
}

void TDeckSpeaker::loop() {
  if (!_ok) return;

  if (_step >= _count) {
    // Nothing left to generate. The only work here is starting a queued alert,
    // and only once this one has actually been heard - see below. The early
    // return this replaced skipped that block entirely, so a queued alert would
    // never have started.
    if (_have_pending && !isDraining()) {
      _have_pending = false;
      Step seq[MAX_STEPS];
      const int n = _pending_count;
      const uint8_t g = _pending_gain;
      for (int i = 0; i < n; i++) seq[i] = _pending[i];
      play(seq, n, g);
    }
    return;
  }

  _passes++;

  // Underrun test, before any writing: at the sample rate the engine consumes
  // exactly _rate samples a second, so by now it must have played this many. If
  // that exceeds what it has been given, it ran out and put silence out instead -
  // which is the stutter, measured rather than reasoned about.
  if (_audio_started_at != 0) {
    uint32_t due = (millis() - _audio_started_at) * (uint32_t) _rate / 1000u;
    if (due > _written_total) _underruns++;
  }

  int16_t buf[SPK_DMA_LEN];

  // Fill whatever room the queue has and then stop. Writing one buffer per pass was
  // not enough: one buffer is 8 ms of audio, so the tone could only ever be fed as
  // fast as the main loop happened to turn over, and any pause in the loop was an
  // audible gap. Bounded by the queue depth, so this is still a short call - the
  // zero timeout is what guarantees that, not the single write.
  for (int fills = 0; fills <= SPK_DMA_COUNT; fills++) {
    if (_step >= _count) break;

    if (_step_done >= _step_samples) {   // this note is fully played
      _step++;
      beginStep();
      continue;
    }

    uint32_t left = _step_samples - _step_done;
    int n = (left < (uint32_t) SPK_DMA_LEN) ? (int) left : SPK_DMA_LEN;

    // Generated from a saved base so the phase can be set from what was actually
    // accepted. Advancing it for samples the queue refused left a discontinuity in
    // the waveform every time the queue was full, which is a click per buffer.
    const uint32_t base = _phase;
    if (_inc == 0) {
      memset(buf, 0, (size_t) n * sizeof(int16_t));    // a rest still costs time
    } else {
      const int32_t amp = SPK_AMPLITUDE * _gain / 100;
      for (int i = 0; i < n; i++) {
        uint32_t ph = base + (uint32_t) i * _inc;
        int32_t v = (int32_t) s_sine[ph >> 24] * amp / 32767;

        // Ramp in and out over a few milliseconds.
        //
        // Without this the waveform went from silence to full amplitude in one
        // sample and back again at the end of every note, and a step that size is
        // a click. Two clicks per note, plus one at each pitch change, is most of
        // what "the sound is odd and interrupted" was: the tone was intact and the
        // edges around it were not.
        //
        // Linear, over SPK_RAMP samples at each end of the note. At 16 kHz that is
        // 4 ms, short enough not to soften a 100 ms alert and long enough that the
        // step per sample is inaudible.
        const uint32_t pos = _step_done + (uint32_t) i;
        const uint32_t remain = _step_samples > pos ? _step_samples - pos : 0;
        if (pos < SPK_RAMP)    v = v * (int32_t) pos / SPK_RAMP;
        if (remain < SPK_RAMP) v = v * (int32_t) remain / SPK_RAMP;
        buf[i] = (int16_t) v;
      }
    }

    size_t written = 0;
    if (i2s_write(SPK_PORT, buf, (size_t) n * sizeof(int16_t), &written, 0) != ESP_OK) break;

    const uint32_t did = (uint32_t) (written / sizeof(int16_t));
    // When the first sample of this sequence actually reached the driver, not when
    // play() was called. The two differ by however long the loop took to come
    // back, and the drain deadline has to be measured from the audio.
    if (did > 0 && _audio_started_at == 0) _audio_started_at = millis();
    _phase = base + did * _inc;
    _step_done += did;
    _frames += did;
    _written_total += did;

    if (did < (uint32_t) n) break;    // queue full; come back next pass
  }

  if (_step >= _count && !_ev_ready) {
    // Recorded when generation finishes, which is also when the previous tone's
    // end becomes known: _audio_started_at + _total_ms is when this one stops
    // sounding, and the difference from the last one is the silence between them.
    _ev_samples = _written_total;
    _ev_passes = _passes;
    _ev_silence_ms = (_prev_end_ms != 0 && _audio_started_at > _prev_end_ms)
                       ? (_audio_started_at - _prev_end_ms) : 0;
    _prev_end_ms = _audio_started_at + _total_ms;
    _ev_ready = true;
  }

  // Deliberately does NOT zero the buffers here.
  //
  // This ran the moment the generator had written its last sample, which is not
  // the moment the audio has been heard: everything still queued was thrown away.
  // At a 32 ms queue that clipped the tail, which is what "cut off" was. When the
  // loop was idle and fast enough to queue a whole short tone in one pass, it
  // deleted the tone before a note of it played - which is what "sometimes no
  // sound" was. A bigger queue would have made that the normal case.
  //
  // Nothing needs zeroing: tx_desc_auto_clear is set, so the driver emits silence
  // once the queue drains rather than repeating the last buffer. The queue is left
  // to play out, and the session is only marked finished once it has - see
  // isPlaying(). stop() still zeroes, because that is an explicit cut.
}

#else   // not ESP32

bool TDeckSpeaker::begin(int, int, int, int) { return false; }
void TDeckSpeaker::play(const Step*, int, uint8_t) { }
void TDeckSpeaker::beginStep() { }
void TDeckSpeaker::stop() { }
void TDeckSpeaker::loop() { }
bool TDeckSpeaker::isDraining() const { return false; }
uint32_t TDeckSpeaker::bufferedMs() const { return 0; }
bool TDeckSpeaker::takeEvent(char*, int) { return false; }

#endif
