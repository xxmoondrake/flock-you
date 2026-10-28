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
#include "fy_usb.h"

namespace {

using fy::device::Dashboard;
using fy::device::Mode;
using fy::device::Outputs;
using fy::device::State;

const char *TAG = "device";

constexpr uint32_t kIdleLimitMs = CONFIG_FY_DASHBOARD_IDLE_S * 1000u;
constexpr uint32_t kTickMs = 1000;
constexpr int kNoRequest = -1;
constexpr int kStopRequest = static_cast<int>(Dashboard::Off);

TaskHandle_t task_handle = nullptr;

// Requests from other tasks, applied by the device task.
std::atomic<int> requested_mode{kNoRequest};       // a Mode
std::atomic<int> requested_dashboard{kNoRequest};  // a Dashboard; Off stops
std::atomic<bool> activity{false};

portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
fy_device_status published = {Mode::Scanning, Dashboard::Off, false, 0};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void wake() {
  if (task_handle) xTaskNotifyGive(task_handle);
}

const char *dashboard_name(Dashboard d) {
  return d == Dashboard::Wifi ? "wifi" : d == Dashboard::Usb ? "usb" : "off";
}

// The dashboard pattern means detection is paused, so only Wi-Fi sessions
// show it.
fy::status::Mode led_mode(const State &s) {
  if (s.dashboard == Dashboard::Wifi) return fy::status::Mode::Dashboard;
  return s.selected == Mode::Standby ? fy::status::Mode::Standby : fy::status::Mode::Scanning;
}

// Moves the radios and USB port from `have` toward `want` and returns what
// was reached. Detection and the access point share the Wi-Fi radio, so
// everything that stops does so before anything starts.
Outputs apply(Outputs have, const Outputs &want) {
  if (have.usb_dashboard && !want.usb_dashboard) {
    fy_usb_stop();
    have.usb_dashboard = false;
  }
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
  if (!have.usb_dashboard && want.usb_dashboard) {
    const esp_err_t err = fy_usb_start();
    if (err == ESP_OK) {
      have.usb_dashboard = true;
    } else {
      ESP_LOGE(TAG, "USB dashboard failed to start: %s", esp_err_to_name(err));
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
  // The detector starts scanning; a USB dashboard started at boot is not a
  // session and stays outside this state.
  Outputs have = {true, true, false, false};
  bool failed = false;
  fy::status::Mode shown_led = fy::status::Mode::Scanning;
  fy_status_led_set_mode(shown_led);

  for (;;) {
    const uint32_t now = now_ms();
    const State before = state;

    const int mode = requested_mode.exchange(kNoRequest);
    if (mode != kNoRequest) fy::device::select_mode(state, static_cast<Mode>(mode));
    const int dashboard = requested_dashboard.exchange(kNoRequest);
    if (dashboard == kStopRequest) {
      fy::device::stop_dashboard(state);
    } else if (dashboard != kNoRequest && state.dashboard == Dashboard::Off) {
      fy::device::start_dashboard(state, static_cast<Dashboard>(dashboard), now);
      failed = false;
    }
    if (activity.exchange(false)) fy::device::note_activity(state, now);
    if (state.dashboard == Dashboard::Wifi &&
        fy::device::dashboard_remaining_ms(state, now, kIdleLimitMs) == 0) {
      ESP_LOGI(TAG, "Wi-Fi dashboard ending after %d s without use", CONFIG_FY_DASHBOARD_IDLE_S);
      fy::device::stop_dashboard(state);
    }

    have = apply(have, fy::device::outputs(state));
    const Outputs want = fy::device::outputs(state);
    if (have.access_point != want.access_point || have.usb_dashboard != want.usb_dashboard) {
      failed = true;  // a dashboard did not start
      fy::device::stop_dashboard(state);
      have = apply(have, fy::device::outputs(state));
    }

    if (state.selected != before.selected || state.dashboard != before.dashboard) {
      ESP_LOGI(TAG, "mode=%s dashboard=%s",
               state.selected == Mode::Standby ? "standby" : "scanning",
               dashboard_name(state.dashboard));
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

#if CONFIG_FY_WIFI_DASHBOARD
constexpr uint32_t kButtonPollMs = 20;
constexpr int kDebouncePolls = 2;

// Boards with a button but no display: five quick presses start or stop the
// Wi-Fi dashboard. Boards with a display use their menu instead.
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
          const bool running = s.dashboard == Dashboard::Wifi;
          ESP_LOGI(TAG, "five presses: %s the Wi-Fi dashboard", running ? "stopping" : "starting");
          if (running) {
            fy_device_stop_dashboard();
          } else {
            fy_device_start_dashboard(Dashboard::Wifi);
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kButtonPollMs));
  }
}
#endif  // CONFIG_FY_WIFI_DASHBOARD

}  // namespace

esp_err_t fy_device_start() {
  if (task_handle) return ESP_OK;
  fy_dashboard_set_request_hook(fy_device_note_activity);
  fy_dashboard_set_mode_hooks(
      [] {
        fy_device_status s;
        fy_device_get_status(&s);
        return s.selected == fy::device::Mode::Standby;
      },
      [](bool standby) {
        fy_device_select_mode(standby ? fy::device::Mode::Standby : fy::device::Mode::Scanning);
      });
  if (xTaskCreate(device_task, "fy_device", 6144, nullptr, 2, &task_handle) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
#if CONFIG_FY_WIFI_DASHBOARD
  if (fy_board_has_button() && !fy_board_display_available() &&
      xTaskCreate(button_task, "fy_button", 3072, nullptr, 2, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
#endif
  return ESP_OK;
}

void fy_device_select_mode(Mode mode) {
  requested_mode = static_cast<int>(mode);
  wake();
}

void fy_device_start_dashboard(Dashboard kind) {
  if (kind == Dashboard::Off) return;
  requested_dashboard = static_cast<int>(kind);
  wake();
}

void fy_device_stop_dashboard() {
  requested_dashboard = kStopRequest;
  wake();
}

void fy_device_note_activity() { activity = true; }

void fy_device_get_status(fy_device_status *out) {
  portENTER_CRITICAL(&status_lock);
  *out = published;
  portEXIT_CRITICAL(&status_lock);
}
