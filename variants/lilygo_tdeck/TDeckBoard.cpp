#include <Arduino.h>
#include "TDeckBoard.h"

uint32_t deviceOnline = 0x00;

void TDeckBoard::begin() {
  
  ESP32Board::begin();

  // Stated rather than inherited. A full cell is 4.2V and the divider halves it, so
  // the pin sees up to 2.1V and needs the widest attenuation to stay on scale - and
  // analogReadMilliVolts() picks its calibration curve from whatever attenuation is
  // configured, so leaving it at the core's default meant the correction and the
  // range were both being assumed. Four other variants here already say it out loud.
  analogSetPinAttenuation(PIN_VBAT_READ, ADC_11db);

  // Enable peripheral power
  pinMode(PIN_PERF_POWERON, OUTPUT);
  digitalWrite(PIN_PERF_POWERON, HIGH);

  // Configure user button
  pinMode(PIN_USER_BTN, INPUT);

  // Configure LoRa Pins
  pinMode(P_LORA_MISO, INPUT_PULLUP);
  // pinMode(P_LORA_DIO_1, INPUT_PULLUP);

  #ifdef P_LORA_TX_LED
    digitalWrite(P_LORA_TX_LED, HIGH); // inverted pin for SX1276 - HIGH for off
  #endif

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET; // received a LoRa packet (while in deep sleep)
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}