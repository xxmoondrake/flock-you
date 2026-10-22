// SPDX-License-Identifier: MIT

#include "fy_device.h"

#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fy_board.h"
#include "fy_dashboard.h"
#include "fy_detector.h"
#include "fy_status_led.h"

namespace {

using fy::device::Mode;
using fy::device::Outputs;
using fy::device::State;

const char *TAG = "device";

constexpr uint32_t kIdleLimitMs = CONFIG_FY_DASHBOARD_IDLE_S * 1000u;
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kButtonPollMs = 20;
constexpr int kDebouncePolls = 2;

TaskHandle_t task_handle = nullptr;

// Requests from other tasks, applied by the device task.
std::atomic<int> requested_mode{-1};       // -1 none, otherwise a Mode
std::atomic<int> requested_dashboard{-1};  // -1 none, 0 stop, 1 start
std::atomic<bool> activity{false};

portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
fy_device_status published = {Mode::Scanning, false, false, 0};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void wake() {
  if (task_handle) xTaskNotifyGive(task_handle);
}

fy::status::Mode led_mode(const State &s) {
  if (s.dashboard) return fy::status::Mode::Dashboard;
  return s.selected == Mode::Standby ? fy::status::Mode::Standby : fy::status::Mode::Scanning;
}

// Moves the radios from `have` toward `want` and returns what was reached.
// Detection and the access point share the Wi-Fi radio, so everything that
// stops does so before anything starts.
Outputs apply(Outputs have, const Outputs &want) {
  if (have.access_point && !want.access_point) {
    fy_dashboard_stop();
    have.access_point = false;
  }
  if (have.wifi_capture && !want.wifi_capture) {
    fy_detector_set_wifi_capture(false);
    have.wifi_capture = false;
  }
  if (have.ble_scan && !want.ble_scan) {
    fy_detector_set_ble_scan(false);
    have.ble_scan = false;
  }
  if (!have.access_point && want.access_point) {
    const esp_err_t err = fy_dashboard_start();
    if (err == ESP_OK) {
      have.access_point = true;
    } else {
      ESP_LOGE(TAG, "Wi-Fi dashboard failed to start: %s", esp_err_to_name(err));
    }
  }
  if (!have.wifi_capture && want.wifi_capture) {
    fy_detector_set_wifi_capture(true);
    have.wifi_capture = true;
  }
  if (!have.ble_scan && want.ble_scan) {
    fy_detector_set_ble_scan(true);
    have.ble_scan = true;
  }
  return have;
}

void device_task(void *) {
  State state;
  Outputs have = {true, true, false};  // the detector starts scanning
  bool failed = false;
  fy::status::Mode shown_led = fy::status::Mode::Scanning;
  fy_status_led_set_mode(shown_led);

  for (;;) {
    const uint32_t now = now_ms();
    const State before = state;

    const int mode = requested_mode.exchange(-1);
    if (mode >= 0) fy::device::select_mode(state, static_cast<Mode>(mode));
    const int dashboard = requested_dashboard.exchange(-1);
    if (dashboard == 1 && !state.dashboard) {
      fy::device::start_dashboard(state, now);
      failed = false;
    } else if (dashboard == 0) {
      fy::device::stop_dashboard(state);
    }
    if (activity.exchange(false)) fy::device::note_activity(state, now);
    if (state.dashboard && fy::device::dashboard_remaining_ms(state, now, kIdleLimitMs) == 0) {
      ESP_LOGI(TAG, "Wi-Fi dashboard ending after %d s without use", CONFIG_FY_DASHBOARD_IDLE_S);
      fy::device::stop_dashboard(state);
    }

    have = apply(have, fy::device::outputs(state));
    if (state.dashboard && !have.access_point) {
      failed = true;
      fy::device::stop_dashboard(state);
      have = apply(have, fy::device::outputs(state));
    }

    if (state.selected != before.selected || state.dashboard != before.dashboard) {
      ESP_LOGI(TAG, "mode=%s dashboard=%s",
               state.selected == Mode::Standby ? "standby" : "scanning",
               state.dashboard ? "on" : "off");
    }
    const fy::status::Mode led = led_mode(state);
    if (led != shown_led) {
      fy_status_led_set_mode(led);
      shown_led = led;
    }

    portENTER_CRITICAL(&status_lock);
    published.selected = state.selected;
    published.dashboard = state.dashboard;
    published.dashboard_failed = failed;
    published.dashboard_remaining_ms =
        fy::device::dashboard_remaining_ms(state, now, kIdleLimitMs);
    portEXIT_CRITICAL(&status_lock);

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));
  }
}

// Boards with a button but no display (the XIAO): five quick presses start or
// stop the Wi-Fi dashboard. Boards with a display use their menu instead.
void button_task(void *) {
  fy::device::PressCounter counter;
  bool pressed = false;
  int stable = 0;
  uint32_t pressed_since = 0;
  for (;;) {
    const uint32_t now = now_ms();
    const bool raw = fy_board_button_pressed();
    stable = (raw != pressed) ? stable + 1 : 0;
    if (stable >= kDebouncePolls) {
      stable = 0;
      pressed = raw;
      if (pressed) {
        pressed_since = now;
      } else {
        fy_device_note_activity();
        if (fy::device::count_press(counter, now, now - pressed_since)) {
          fy_device_status s;
          fy_device_get_status(&s);
          ESP_LOGI(TAG, "five presses: %s the Wi-Fi dashboard", s.dashboard ? "stopping" : "starting");
          fy_device_set_dashboard(!s.dashboard);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kButtonPollMs));
  }
}

}  // namespace

esp_err_t fy_device_start() {
  if (task_handle) return ESP_OK;
  fy_dashboard_set_request_hook(fy_device_note_activity);
  if (xTaskCreate(device_task, "fy_device", 6144, nullptr, 2, &task_handle) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  if (fy_board_has_button() && !fy_board_display_available() &&
      xTaskCreate(button_task, "fy_button", 3072, nullptr, 2, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void fy_device_select_mode(Mode mode) {
  requested_mode = static_cast<int>(mode);
  wake();
}

void fy_device_set_dashboard(bool on) {
  requested_dashboard = on ? 1 : 0;
  wake();
}

void fy_device_note_activity() { activity = true; }

void fy_device_get_status(fy_device_status *out) {
  portENTER_CRITICAL(&status_lock);
  *out = published;
  portEXIT_CRITICAL(&status_lock);
}
