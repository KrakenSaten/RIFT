#include "TDeckSpeaker.h"

#if defined(ESP32)
#include <driver/i2s.h>
#include <math.h>

// Port 0. Nothing else in this firmware uses I2S, and the mesh radio is on SPI, so
// there is no contention to arbitrate.
#define SPK_PORT   I2S_NUM_0

// Small buffers on purpose. The DMA queue is what decides how long a blocking write
// would block for, and this never blocks - but a long queue would also mean a tone
// kept sounding well after stop(), which reads as a stuck alert.
#define SPK_DMA_COUNT  4
#define SPK_DMA_LEN    128

// A quarter of full scale. This amplifier is loud enough to be unpleasant at full
// amplitude in a room, and an alert has to be noticed rather than resented.
#define SPK_AMPLITUDE  8000

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
  _gain = gain > 100 ? 100 : gain;
  for (int i = 0; i < count; i++) _seq[i] = steps[i];
  _count = count;
  _step = 0;
  beginStep();
}

void TDeckSpeaker::stop() {
  if (!_ok) return;
  _count = _step = 0;
  i2s_zero_dma_buffer(SPK_PORT);
}

void TDeckSpeaker::loop() {
  if (!_ok || _step >= _count) return;

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
      for (int i = 0; i < n; i++) {
        uint32_t ph = base + (uint32_t) i * _inc;
        buf[i] = (int16_t) ((int32_t) s_sine[ph >> 24] * (SPK_AMPLITUDE * _gain / 100) / 32767);
      }
    }

    size_t written = 0;
    if (i2s_write(SPK_PORT, buf, (size_t) n * sizeof(int16_t), &written, 0) != ESP_OK) break;

    const uint32_t did = (uint32_t) (written / sizeof(int16_t));
    _phase = base + did * _inc;
    _step_done += did;
    _frames += did;

    if (did < (uint32_t) n) break;    // queue full; come back next pass
  }

  if (_step >= _count) {
    // Zero the buffers on the way out, or the tail of the last note sits in DMA and
    // repeats until something else writes.
    i2s_zero_dma_buffer(SPK_PORT);
  }
}

#else   // not ESP32

bool TDeckSpeaker::begin(int, int, int, int) { return false; }
void TDeckSpeaker::play(const Step*, int, uint8_t) { }
void TDeckSpeaker::beginStep() { }
void TDeckSpeaker::stop() { }
void TDeckSpeaker::loop() { }

#endif
