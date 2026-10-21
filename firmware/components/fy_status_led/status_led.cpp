// SPDX-License-Identifier: MIT

#include "fy_status_led.h"

#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fy_board.h"

namespace {

using fy::status::LedState;
using fy::status::Mode;

const char *TAG = "status_led";

TaskHandle_t led_task_handle = nullptr;
std::atomic<Mode> requested_mode{Mode::Booting};
std::atomic<bool> match_requested{false};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

LedState base_state(Mode mode, uint32_t elapsed_ms) {
#if CONFIG_FY_STATUS_LED_PATTERNS
  return fy::status::mode_state(mode, elapsed_ms);
#else
  (void)mode;
  (void)elapsed_ms;
  return {false, 0};
#endif
}

// Recomputes the LED from the time since the mode (or match burst) began, so
// late wake-ups never accumulate drift, then sleeps until the next change or
// a notification.
void led_task(void *) {
  Mode shown = requested_mode.load();
  uint32_t mode_since = now_ms();
  bool in_match = false;
  uint32_t match_since = 0;
  for (;;) {
    const uint32_t now = now_ms();
    const Mode mode = requested_mode.load();
    if (mode != shown) {
      shown = mode;
      mode_since = now;
    }
    if (match_requested.exchange(false)) {
      in_match = true;
      match_since = now;
    }
    if (in_match && now - match_since >= fy::status::kMatchBurstMs) in_match = false;

    const LedState s = in_match ? fy::status::match_state(now - match_since)
                                : base_state(shown, now - mode_since);
    fy_board_led_set(s.on);
    ulTaskNotifyTake(pdTRUE, s.hold_ms ? pdMS_TO_TICKS(s.hold_ms) : portMAX_DELAY);
  }
}

}  // namespace

esp_err_t fy_status_led_start() {
  if (!fy_board_has_led()) return ESP_ERR_NOT_SUPPORTED;
  if (led_task_handle) return ESP_OK;
  if (xTaskCreate(led_task, "fy_led", 2048, nullptr, 1, &led_task_handle) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
#if CONFIG_FY_STATUS_LED_PATTERNS
  ESP_LOGI(TAG, "started with mode patterns");
#else
  ESP_LOGI(TAG, "started for match flashes only");
#endif
  return ESP_OK;
}

void fy_status_led_set_mode(Mode mode) {
  requested_mode = mode;
  if (led_task_handle) xTaskNotifyGive(led_task_handle);
}

void fy_status_led_indicate_match() {
  if (!led_task_handle) return;
  match_requested = true;
  xTaskNotifyGive(led_task_handle);
}
