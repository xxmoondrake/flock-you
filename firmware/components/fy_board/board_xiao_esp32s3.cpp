// SPDX-License-Identifier: MIT
//
// Seeed Studio XIAO ESP32S3. The user LED is GPIO21 and active-low (Seeed
// board definition). The
// BOOT button is GPIO0, active low; held during reset it selects the serial
// bootloader, and after boot it is read as the board's only button. The
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
constexpr gpio_num_t kButtonPin = GPIO_NUM_0;  // BOOT, active low
bool led_ready = false;
bool button_ready = false;

void led_write(bool on) { gpio_set_level(kLedPin, on ? kLedOnLevel : !kLedOnLevel); }

}  // namespace

void fy_board_init() {
  gpio_config_t led = {};
  led.pin_bit_mask = 1ULL << kLedPin;
  led.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&led));
  led_write(false);
  led_ready = true;

  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << kButtonPin;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&button));
  button_ready = true;
  ESP_LOGI(TAG, "XIAO ESP32S3: LED on GPIO%d, BOOT button on GPIO%d", static_cast<int>(kLedPin),
           static_cast<int>(kButtonPin));
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

// No screens to navigate: five quick BOOT presses start or stop the Wi-Fi
// dashboard (fy_device).
bool fy_board_has_button() { return true; }

bool fy_board_button_pressed() { return button_ready && gpio_get_level(kButtonPin) == 0; }

void fy_board_power_off(uint32_t timer_wake_s) {
  if (led_ready) led_write(false);
  if (timer_wake_s) esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_wake_s) * 1000000ULL);
  esp_deep_sleep_start();  // otherwise woken only by a reset or power cycle
}

#endif  // CONFIG_FY_BOARD_XIAO_ESP32S3
