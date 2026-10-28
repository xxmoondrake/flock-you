// SPDX-License-Identifier: MIT

#include "fy_ui.h"

#include <atomic>
#include <cstdio>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fy_alerts.h"
#include "fy_battery.h"
#include "fy_board.h"
#include "fy_canvas.h"
#include "fy_dashboard.h"
#include "fy_detector.h"
#include "fy_device.h"
#include "fy_gps.h"
#include "fy_recorder.h"
#include "fy_signatures.h"
#include "fy_store.h"
#include "fy_ui_alerts.h"
#include "fy_ui_controller.h"
#include "fy_ui_layout.h"
#include "fy_usb.h"
#include "splash_frame.h"

namespace {

using fy::ui::Action;
using fy::ui::UiModel;
using fy::ui::UiState;

const char *TAG = "ui";

constexpr uint32_t kRefreshMs = 500;
constexpr uint32_t kPollMs = 20;
constexpr int kDebouncePolls = 2;
// How long a specific match keeps the alert banner on screen.
constexpr uint32_t kAlertMs = 10000;
constexpr uint32_t kPowerOffSaveTimeoutMs = 2000;
constexpr uint32_t kShutdownNoticeMs = 500;  // the power-off screen's time after saving
constexpr uint32_t kSaveFailedNoticeMs = 1500;  // shown before powering off anyway
constexpr uint32_t kLowBatteryNoticeMs = 3000;
constexpr uint32_t kSplashMinMs = 3000;
constexpr uint32_t kAboutMs = 2000;

fy::display::Canvas canvas;
UiModel model;  // large; kept off the task stack
UiState state;

std::atomic<bool> ui_running{false};
std::atomic<bool> low_battery_requested{false};
std::atomic<uint16_t> low_battery_mv{0};
uint32_t splash_until_ms = 0;  // set before the UI task starts

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Device types with at least one device: specific types first, then by
// stored count.
void gather_detections(UiModel &m, const fy_recorder_status &r) {
  m.detection_rows = 0;
  for (int t = 0; t < static_cast<int>(fy::DeviceType::Count); t++) {
    if (!r.session_devices[t] && !r.stored_devices[t]) continue;
    if (m.detection_rows >= fy::ui::kMaxDetectionRows) break;
    const fy::DeviceTypeInfo &info = fy::device_type_info(static_cast<fy::DeviceType>(t));
    fy::ui::DetectionRow row = {info.name, info.possible, r.session_devices[t],
                                r.stored_devices[t]};
    int i = m.detection_rows++;
    while (i > 0) {
      const fy::ui::DetectionRow &prev = m.detections[i - 1];
      const bool before = (!row.possible && prev.possible) ||
                          (row.possible == prev.possible && row.stored > prev.stored);
      if (!before) break;
      m.detections[i] = prev;
      i--;
    }
    m.detections[i] = row;
  }
  m.counts_saturated = r.session_saturated || r.stored_saturated;
  m.stored_pending = r.stored_pending;
}

void gather(UiModel &m, bool power_off_armed) {
  m = UiModel{};
  m.now_ms = now_ms();
  m.power_off_armed = power_off_armed;
  const fy::alerts::Settings alerts = fy_alerts_settings();
  m.alerts_enabled = alerts.enabled;
  m.alert_buzzer = alerts.buzzer;
  m.alert_flash = alerts.flash;
  m.buzzer_fitted = fy_board_has_buzzer();

  fy_detector_status d;
  fy_detector_get_status(&d);
  m.wifi_capture = d.wifi_capture;
  m.ble_scan = d.ble_scan;
  m.channel = d.channel;

  fy_device_status dev;
  fy_device_get_status(&dev);
  m.standby = dev.selected == fy::device::Mode::Standby;
  m.dashboard_active = dev.dashboard != fy::device::Dashboard::Off;
  m.dashboard_usb = dev.dashboard == fy::device::Dashboard::Usb;
  m.dashboard_failed = dev.dashboard_failed;
  m.dashboard_left_ms = dev.dashboard_remaining_ms;
  if (dev.dashboard == fy::device::Dashboard::Wifi) {
    fy_dashboard_status dash;
    fy_dashboard_get_status(&dash);
    snprintf(m.dashboard_ssid, sizeof(m.dashboard_ssid), "%s", dash.ssid);
    snprintf(m.dashboard_passkey, sizeof(m.dashboard_passkey), "%s", dash.passkey);
    snprintf(m.dashboard_address, sizeof(m.dashboard_address), "%s", dash.address);
    m.dashboard_clients = dash.clients;
  } else if (dev.dashboard == fy::device::Dashboard::Usb) {
    snprintf(m.dashboard_address, sizeof(m.dashboard_address), "%s", FY_USB_ADDRESS);
  }
  m.reported = d.reported;
  m.reported_specific = d.reported_specific;
  m.last_valid = d.last.valid;
  m.last_rule = d.last.rule_id;
  m.last_specific = d.last.specific;
  m.last_rssi = d.last.rssi;
  m.last_at_ms = d.last.at_ms;
  if (d.last_specific.valid) {
    m.alert_age_ms = m.now_ms - d.last_specific.at_ms;
    m.alert_active = m.alert_age_ms < kAlertMs;
  }

  fy_gps_status g;
  fy_gps_get_status(&g);
  m.gps_available = g.available;
  m.gps_receiving = g.receiving;
  m.gps_time_valid = g.nav.time_valid && g.time_age_ms != UINT32_MAX;
  m.utc_hour = g.nav.utc.hour;
  m.utc_minute = g.nav.utc.minute;
  m.utc_second = g.nav.utc.second;
  m.gps_time_age_ms = g.time_age_ms;
  m.gps_position_current = g.position_current;
  m.gps_position_ever = g.position_age_ms != UINT32_MAX;
  m.gps_position_age_ms = g.position_age_ms;
  m.latitude_deg = g.nav.latitude_deg;
  m.longitude_deg = g.nav.longitude_deg;
  m.fix_type = g.nav.fix_type;
  m.satellites = g.nav.satellites;
  m.hdop_valid = g.nav.hdop_valid;
  m.hdop = g.nav.hdop;
  m.sats_in_view = g.nav.satellites_in_view;
  m.sats_with_signal = g.nav.satellites_with_signal;
  m.antenna = static_cast<uint8_t>(g.nav.antenna);

  fy_battery_status b;
  fy_battery_get_status(&b);
  m.battery_available = b.available;
  m.battery_valid = b.valid;
  m.battery_mv = b.mv;
  m.battery_percent = b.percent;

  fy_recorder_status r;
  fy_recorder_get_status(&r);
  m.storage_available = r.available;
  m.storage_warning = r.available && r.storage_warning;
  m.storage_percent = r.used_percent;
  m.records_used = r.used;
  m.records_capacity = r.capacity;
  m.encounter_records = r.encounter_records;
  m.oldest_utc = r.oldest_utc;
  m.boot = r.boot;
  m.erasing = r.erasing;
  m.erase_percent = r.erase_percent;
  gather_detections(m, r);
}

void draw(bool power_off_armed) {
  gather(model, power_off_armed);
  fy::ui::render(canvas, model, state);
  fy_board_display_flush(canvas.frame());
}

// Saves open encounters before power-off, within a fixed time. A failed or
// unconfirmed save is logged and shown briefly; power-off goes ahead.
void save_before_power_off() {
  const fy_recorder_save_result result = fy_recorder_save_and_wait(kPowerOffSaveTimeoutMs);
  if (result == FY_RECORDER_SAVED) return;
  const bool failed = result == FY_RECORDER_SAVE_FAILED;
  ESP_LOGW(TAG, "%s before power-off",
           failed ? "saving open encounters failed" : "open encounters not confirmed saved");
  canvas.clear();
  canvas.draw_text(0, 28, failed ? "Save failed" : "Save not confirmed");
  fy_board_display_flush(canvas.frame());
  vTaskDelay(pdMS_TO_TICKS(kSaveFailedNoticeMs));
}

[[noreturn]] void power_off() {
  fy::ui::render_shutdown(canvas);
  fy_board_display_flush(canvas.frame());
  save_before_power_off();
  vTaskDelay(pdMS_TO_TICKS(kShutdownNoticeMs));
  fy_board_power_off();
}

void ui_task(void *) {
  // The start-up image keeps the screen for its minimum time, then the name
  // and versions show briefly.
  if (splash_until_ms) {
    const uint32_t started = now_ms();
    if (splash_until_ms > started) vTaskDelay(pdMS_TO_TICKS(splash_until_ms - started));
    fy::ui::render_about(canvas, fy_board_model(), esp_app_get_description()->version);
    fy_board_display_flush(canvas.frame());
    vTaskDelay(pdMS_TO_TICKS(kAboutMs));
  }

  bool pressed = false;
  int stable = 0;
  uint32_t pressed_since = 0;
  bool armed = false;
  uint32_t last_draw = 0;
  uint32_t save_marker = 0;
  bool save_pending = false;
  bool was_erasing = false;

  // Screen flashes, for specific matches reported from here on and for
  // tests; the buzzer's sounds are fy_alerts'.
  fy::ui::AlertGate alert_gate;
  fy_detector_status det;
  fy_detector_get_status(&det);
  uint32_t seen_specific = det.reported_specific;
  uint32_t seen_tests = fy_alerts_tests();
  bool flashing = false;
  uint32_t flash_started = 0;
  fy::ui::FlashFrame flash_shown = fy::ui::FlashFrame::Done;
  auto start_flash = [&](uint32_t at) {
    flashing = true;
    flash_started = at;
    flash_shown = fy::ui::FlashFrame::Done;
  };

  for (;;) {
    if (low_battery_requested) {
      // Battery protection: warn, save, and power off with a timed re-check.
      fy::ui::render_low_battery(canvas, low_battery_mv, false);
      fy_board_display_flush(canvas.frame());
      vTaskDelay(pdMS_TO_TICKS(kLowBatteryNoticeMs));
      save_before_power_off();
      fy_board_power_off(CONFIG_FY_BATTERY_RECHECK_S);
    }

    const uint32_t now = now_ms();
    bool redraw = now - last_draw >= kRefreshMs;

    // Debounced button with press-duration classification on release.
    const bool raw = fy_board_has_button() && fy_board_button_pressed();
    stable = (raw != pressed) ? stable + 1 : 0;
    if (stable >= kDebouncePolls) {
      stable = 0;
      pressed = raw;
      if (pressed) {
        pressed_since = now;
      } else if (flashing) {
        flashing = false;  // a press during an alert only ends it
        armed = false;
        redraw = true;
      } else {
        const fy::ui::Press press = fy::ui::classify_press(now - pressed_since, now);
        armed = false;
        fy_device_note_activity();  // keeps a dashboard session open
        fy_device_status now_dev;
        fy_device_get_status(&now_dev);
        state.dashboard_running = now_dev.dashboard != fy::device::Dashboard::Off;
        const Action action = fy::ui::handle_press(state, press, now);
        if (action == Action::PowerOff) power_off();
        if (action == Action::SelectScanning || action == Action::SelectStandby) {
          const bool standby = action == Action::SelectStandby;
          fy_device_select_mode(standby ? fy::device::Mode::Standby : fy::device::Mode::Scanning);
          fy_device_status dev;
          fy_device_get_status(&dev);
          const char *notice = dev.dashboard == fy::device::Dashboard::Wifi ? "After dashboard ends"
                               : standby     ? "Standby: not scanning"
                                             : "Scanning";
          fy::ui::show_message(state, notice, now);
        }
        if (action == Action::StopDashboard) {
          fy_device_stop_dashboard();
          fy::ui::show_message(state, "Dashboard stopping", now);
        }
        if (action == Action::StartUsbDashboard) {
          fy_device_start_dashboard(fy::device::Dashboard::Usb);
          fy::ui::show_message(state, "USB dashboard on", now);
        }
        if (action == Action::StartWifiDashboard) {
          fy_device_start_dashboard(fy::device::Dashboard::Wifi);
          fy::ui::show_message(state, "WiFi dashboard on", now);
        }
        if (action == Action::SaveOpen) {
          save_marker = fy_recorder_request_save();
          save_pending = true;
          fy::ui::show_message(state, "Saving encounters", now);
        }
        if (action == Action::EraseAll) fy_recorder_request_erase();
        if (action == Action::ToggleAlerts || action == Action::ToggleBuzzer ||
            action == Action::ToggleFlash) {
          fy::alerts::Settings s = fy_alerts_settings();
          bool &setting = action == Action::ToggleAlerts   ? s.enabled
                          : action == Action::ToggleBuzzer ? s.buzzer
                                                           : s.flash;
          setting = !setting;
          if (!fy_alerts_set(s)) fy::ui::show_message(state, "Setting not saved", now);
        }
        if (action == Action::TestAlert) {
          const fy::alerts::Settings s = fy_alerts_settings();
          if (!s.enabled) {
            fy::ui::show_message(state, "Alerts are off", now);
          } else if (!s.flash_on() && !(s.buzzer_on() && fy_board_has_buzzer())) {
            fy::ui::show_message(state, "No alert output", now);
          } else {
            fy_alerts_test();  // the buzzer sounds; the flash follows below
          }
        }
        redraw = true;
      }
    }
    if (pressed) {
      const bool now_armed = now - pressed_since >= fy::ui::kPowerOffHoldMs &&
                             now >= fy::ui::kPowerOffMinUptimeMs;
      if (now_armed != armed) {
        armed = now_armed;
        redraw = true;
      }
    }

    // Completion notices for requested actions.
    fy_recorder_status r;
    fy_recorder_get_status(&r);
    if (save_pending && fy::store::SaveLedger::answers(r.save_answered, save_marker)) {
      save_pending = false;
      fy::ui::show_message(state, r.last_save_ok ? "Encounters saved" : "Save failed", now);
      redraw = true;
    }
    if (was_erasing && !r.erasing) {
      fy::ui::show_message(state, r.last_erase_ok ? "Records erased" : "Erase had errors", now);
      redraw = true;
    }
    if (r.erasing) redraw = redraw || now - last_draw >= 200;
    was_erasing = r.erasing;

    // A newly reported specific match flashes the screen unless its address
    // flashed it recently, and so does a test (from here or the dashboard).
    // The image covers the screen briefly, then the screen returns.
    fy_detector_get_status(&det);
    const bool flash_on = fy_alerts_settings().flash_on();
    if (det.reported_specific != seen_specific) {
      seen_specific = det.reported_specific;
      if (flash_on && alert_gate.admit(det.last_specific.mac, now)) start_flash(now);
    }
    const uint32_t tests = fy_alerts_tests();
    if (tests != seen_tests) {
      seen_tests = tests;
      if (flash_on) start_flash(now);
    }
    if (flashing) {
      const fy::ui::FlashFrame frame = fy::ui::flash_frame(now - flash_started);
      if (frame == fy::ui::FlashFrame::Done) {
        flashing = false;
        redraw = true;
      } else {
        if (frame != flash_shown) {
          canvas.load_frame(fy::ui::kSplashFrame, frame == fy::ui::FlashFrame::Inverted);
          fy_board_display_flush(canvas.frame());
          flash_shown = frame;
        }
        redraw = false;
      }
    }

    if (redraw) {
      draw(armed);
      last_draw = now;
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
}

}  // namespace

void fy_ui_show_splash() {
  if (!fy_board_display_available()) return;
  fy_board_display_flush(fy::ui::kSplashFrame);
  splash_until_ms = now_ms() + kSplashMinMs;
}

esp_err_t fy_ui_start() {
  if (!fy_board_display_available()) return ESP_ERR_NOT_SUPPORTED;
  if (xTaskCreate(ui_task, "fy_ui", 6144, nullptr, 2, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
  ui_running = true;
  ESP_LOGI(TAG, "screens started (button: %s)", fy_board_has_button() ? "yes" : "no");
  return ESP_OK;
}

void fy_ui_show_low_battery_blocking(uint16_t mv, bool starting, uint32_t duration_ms) {
  if (!fy_board_display_available()) return;
  fy::ui::render_low_battery(canvas, mv, starting);
  fy_board_display_flush(canvas.frame());
  vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

bool fy_ui_request_low_battery_power_off(uint16_t mv) {
  if (!ui_running) return false;
  low_battery_mv = mv;
  low_battery_requested = true;
  return true;
}
