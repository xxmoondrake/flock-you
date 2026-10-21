// SPDX-License-Identifier: MIT
//
// Seeed Studio XIAO ESP32S3. The user LED is GPIO21 and active-low (Seeed
// board definition). The
// board has no display. The GPS receiver (planned on UART0, D7/GPIO44 as RX)
// stays disabled until the breakout's identity, supply voltage, and UART
// levels are confirmed.

#include "sdkconfig.h"

#if CONFIG_FY_BOARD_XIAO_ESP32S3

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "fy_board.h"

namespace {

const char *TAG = "board";

constexpr gpio_num_t kLedPin = GPIO_NUM_21;
constexpr int kLedOnLevel = 0;  // active-low
bool led_ready = false;

void led_write(bool on) { gpio_set_level(kLedPin, on ? kLedOnLevel : !kLedOnLevel); }

}  // namespace

void fy_board_init() {
  gpio_config_t led = {};
  led.pin_bit_mask = 1ULL << kLedPin;
  led.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&led));
  led_write(false);
  led_ready = true;
  ESP_LOGI(TAG, "XIAO ESP32S3: LED on GPIO%d", static_cast<int>(kLedPin));
}

const char *fy_board_name() { return "xiao_esp32s3"; }

bool fy_board_has_led() { return true; }

void fy_board_led_set(bool on) {
  if (led_ready) led_write(on);
}

bool fy_board_display_available() { return false; }

void fy_board_display_flush(const uint8_t *) {}

bool fy_board_gps_config_get(fy_board_gps_config *) { return false; }

// Powered by the attached phone or PC; no battery in this phase.
bool fy_board_battery_config_get(fy_board_battery_config *) { return false; }

// No on-device navigation: the XIAO is operated through its dashboard. Its
// BOOT button is reserved for the planned five-press Wi-Fi activation.
bool fy_board_has_button() { return false; }

bool fy_board_button_pressed() { return false; }

void fy_board_power_off(uint32_t timer_wake_s) {
  if (led_ready) led_write(false);
  if (timer_wake_s) esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_wake_s) * 1000000ULL);
  esp_deep_sleep_start();  // otherwise woken only by a reset or power cycle
}

#endif  // CONFIG_FY_BOARD_XIAO_ESP32S3
