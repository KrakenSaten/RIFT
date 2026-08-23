#include "TDeckSpeaker.h"

#if defined(ESP32)
#include <driver/i2s.h>

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

// Sixteen points is coarse, but the harmonics of a coarse sine are far kinder than
// the square wave the alternative would be, and this is an alert tone rather than
// music.
static const int16_t SINE16[16] = {
      0,  12539,  23170,  30273,  32767,  30273,  23170,  12539,
      0, -12539, -23170, -30273, -32767, -30273, -23170, -12539
};

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
  _ok = true;
  return true;
}

void TDeckSpeaker::play(const Step* steps, int count) {
  if (!_ok || steps == NULL || count <= 0) return;
  if (count > MAX_STEPS) count = MAX_STEPS;
  for (int i = 0; i < count; i++) _seq[i] = steps[i];
  _count = count;
  _step = 0;
  _phase = 0;
  _step_started = millis();
}

void TDeckSpeaker::stop() {
  if (!_ok) return;
  _count = _step = 0;
  i2s_zero_dma_buffer(SPK_PORT);
}

void TDeckSpeaker::loop() {
  if (!_ok || _step >= _count) return;

  // advance past any finished steps before generating anything
  while (_step < _count && millis() - _step_started >= _seq[_step].ms) {
    _step_started += _seq[_step].ms;
    _step++;
    _phase = 0;
  }
  if (_step >= _count) {
    // Zero the buffers on the way out. Without this the tail of the last tone sits
    // in DMA and repeats until something else writes.
    i2s_zero_dma_buffer(SPK_PORT);
    return;
  }

  const uint16_t hz = _seq[_step].hz;

  // One DMA buffer's worth per pass. Writing more would only sit in a queue, and
  // the point of the zero timeout is that this call is bounded.
  int16_t buf[SPK_DMA_LEN];
  if (hz == 0) {
    memset(buf, 0, sizeof(buf));      // a rest is silence, not a pause in writing
  } else {
    // 16.16 fixed point: phase step per sample, wrapped into the 16-entry table
    const uint32_t inc = (uint32_t) ((((uint64_t) hz) << 16) * 16 / (uint32_t) _rate);
    for (int i = 0; i < SPK_DMA_LEN; i++) {
      buf[i] = (int16_t) ((int32_t) SINE16[(_phase >> 16) & 15] * SPK_AMPLITUDE / 32767);
      _phase += inc;
    }
  }

  size_t written = 0;
  // Zero ticks: take what the queue has room for and return. A partial write is
  // normal and correct - the phase has already advanced for the samples generated,
  // so the discarded tail costs a fraction of a cycle rather than a click.
  if (i2s_write(SPK_PORT, buf, sizeof(buf), &written, 0) == ESP_OK) {
    _frames += (uint32_t) (written / sizeof(int16_t));
  }
}

#else   // not ESP32

bool TDeckSpeaker::begin(int, int, int, int) { return false; }
void TDeckSpeaker::play(const Step*, int) { }
void TDeckSpeaker::stop() { }
void TDeckSpeaker::loop() { }

#endif
