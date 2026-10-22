// SPDX-License-Identifier: MIT
//
// Entry point for the shared detector firmware. Board-specific behavior lives
// in fy_board; capture, matching, and reporting in fy_detector; the GNSS
// receiver in fy_gps; the battery monitor in fy_battery; storage in
// fy_recorder; the screens in fy_ui; the status LED in fy_status_led; modes
// in fy_device; the Wi-Fi dashboard in fy_dashboard; and the USB dashboard
// in fy_usb.

#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "fy_battery.h"
#include "fy_board.h"
#include "fy_detector.h"
#include "fy_device.h"
#include "fy_gps.h"
#include "fy_recorder.h"
#include "fy_status_led.h"
#include "fy_ui.h"
#include "fy_usb.h"

static const char *TAG = "main";

constexpr uint32_t kLowBatteryNoticeMs = 3000;
constexpr uint32_t kSaveTimeoutMs = 2000;

// NVS holds Wi-Fi PHY calibration and the boot session counter. A partition
// from an older or different firmware layout is erased and reinitialized
// rather than treated as fatal.
static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

static const char *reset_reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    default: return "other";
  }
}

static void optional(const char *what, esp_err_t err) {
  if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGE(TAG, "%s start failed: %s", what, esp_err_to_name(err));
  }
}

// Battery protection at start-up, before the display, radios, GNSS, or the
// boot session counter: below the start minimum the board does not start.
// A timed re-check stays dark; a power-on or button wake shows the notice.
static void refuse_start_on_low_battery() {
  uint16_t mv = 0;
  if (fy_battery_measure_once(&mv) != ESP_OK) return;  // no battery measurement
  if (!fy_battery_plausible(mv)) return;  // no battery fitted: running from USB
  if (mv >= CONFIG_FY_BATTERY_BOOT_MIN_MV) return;

  ESP_LOGW(TAG, "battery %u mV below start minimum %d mV; staying off", mv,
           CONFIG_FY_BATTERY_BOOT_MIN_MV);
  const uint32_t causes = esp_sleep_get_wakeup_causes();
  const bool timed_recheck = (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) &&
                             !(causes & (1u << ESP_SLEEP_WAKEUP_EXT0));
  if (!timed_recheck) {
    fy_board_init();
    fy_ui_show_low_battery_blocking(mv, true, kLowBatteryNoticeMs);
  }
  fy_board_power_off(CONFIG_FY_BATTERY_RECHECK_S);
}

// Battery protection while running: the battery stayed below the cutoff.
static void on_battery_critical(uint16_t mv) {
  if (fy_ui_request_low_battery_power_off(mv)) return;  // the UI warns, saves, powers off
  fy_recorder_save_and_wait(kSaveTimeoutMs);
  fy_board_power_off(CONFIG_FY_BATTERY_RECHECK_S);
}

extern "C" void app_main(void) {
  refuse_start_on_low_battery();

  init_nvs();
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  fy_board_init();
  ESP_LOGI(TAG, "board=%s reset=%s", fy_board_name(), reset_reason_name(esp_reset_reason()));
  optional("status LED", fy_status_led_start());  // solid until scanning starts

  // GPS, battery measurement, storage, and the display are optional per
  // board; detection runs without them. The recorder starts before the
  // detector so no sighting is missed.
  optional("GPS", fy_gps_start());
  optional("battery monitor", fy_battery_start());
  fy_battery_set_critical_handler(on_battery_critical);
  optional("recorder", fy_recorder_start());
  ESP_ERROR_CHECK(fy_detector_start());
  // Every boot starts scanning with no dashboard session.
  optional("device modes", fy_device_start());
#if CONFIG_FY_USB_DASHBOARD_AT_BOOT
  // The console moves to the USB serial port from here on.
  optional("USB dashboard", fy_usb_start());
#endif
  optional("display", fy_ui_start());
}
