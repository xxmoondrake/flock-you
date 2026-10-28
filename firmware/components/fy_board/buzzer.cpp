// SPDX-License-Identifier: MIT
//
// Optional alert buzzer, the same on every board: a passive piezo on
// FY_BUZZER_GPIO, driven with a square wave as upstream's tone() does. No
// pin, no buzzer.

#include "fy_board.h"

#include "sdkconfig.h"

#if CONFIG_FY_BUZZER_GPIO >= 0
#include "driver/ledc.h"
#endif

bool fy_board_has_buzzer() { return CONFIG_FY_BUZZER_GPIO >= 0; }

#if CONFIG_FY_BUZZER_GPIO >= 0

namespace {

constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kHalfDuty = 1u << 9;  // a square wave at 10 bits

bool ready = false;

bool set_up() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = kMode;
  timer.duty_resolution = kResolution;
  timer.timer_num = kTimer;
  timer.freq_hz = 2000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer) != ESP_OK) return false;
  ledc_channel_config_t channel = {};
  channel.gpio_num = CONFIG_FY_BUZZER_GPIO;
  channel.speed_mode = kMode;
  channel.channel = kChannel;
  channel.timer_sel = kTimer;
  channel.duty = 0;
  return ledc_channel_config(&channel) == ESP_OK;
}

}  // namespace

void fy_board_buzzer_tone(uint16_t hz) {
  if (!ready && !(ready = set_up())) return;
  if (hz) ledc_set_freq(kMode, kTimer, hz);
  ledc_set_duty(kMode, kChannel, hz ? kHalfDuty : 0);
  ledc_update_duty(kMode, kChannel);
}

#else

void fy_board_buzzer_tone(uint16_t) {}

#endif
