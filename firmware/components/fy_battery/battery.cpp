// SPDX-License-Identifier: MIT

#include "fy_battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "fy_battery_curve.h"
#include "fy_board.h"

namespace {

const char *TAG = "battery";

constexpr uint32_t kIntervalMs = 5000;
constexpr uint32_t kLogIntervalMs = 60000;
constexpr int kSamples = 16;
constexpr uint32_t kSettleMs = 10;  // after connecting the divider
// Outside this range the reading is not a connected single-cell battery
// (for example, no battery fitted).
constexpr uint32_t kMinPlausibleMv = 2500;
constexpr uint32_t kMaxPlausibleMv = 4600;
// Consecutive below-cutoff samples (at kIntervalMs) before shutting down, so
// a brief load dip does not power the board off.
constexpr int kCriticalSamples = 6;

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
fy_battery_status status = {};  // guarded by `lock`

fy_board_battery_config config = {};
adc_oneshot_unit_handle_t adc = nullptr;
adc_channel_t channel = ADC_CHANNEL_0;
adc_cali_handle_t cali = nullptr;
fy_battery_critical_handler critical_handler = nullptr;

void divider_connect(bool on) {
  if (config.enable_gpio < 0) return;
  const int level = on ? config.enable_level : !config.enable_level;
  gpio_set_level(static_cast<gpio_num_t>(config.enable_gpio), level);
}

// Sets up the ADC and the divider enable pin once. Safe to call before the
// rest of the board is initialized, which the boot-time check relies on.
esp_err_t ensure_adc() {
  if (adc) return ESP_OK;
  if (!fy_board_battery_config_get(&config)) return ESP_ERR_NOT_SUPPORTED;

  adc_unit_t unit;
  ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(config.sense_gpio, &unit, &channel), TAG,
                      "sense pin is not an ADC input");
  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = unit;
  ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &adc), TAG, "adc unit");
  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = static_cast<adc_atten_t>(config.adc_atten);
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(adc, channel, &chan_cfg), TAG, "adc channel");
  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = unit;
  cali_cfg.chan = channel;
  cali_cfg.atten = chan_cfg.atten;
  cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali), TAG,
                      "adc calibration");

  if (config.enable_gpio >= 0) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << config.enable_gpio;
    io.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "divider enable pin");
    divider_connect(false);
  }
  return ESP_OK;
}

// Returns the battery voltage in mV, or 0 if the ADC read failed.
uint32_t measure_mv() {
  divider_connect(true);
  vTaskDelay(pdMS_TO_TICKS(kSettleMs));
  int64_t sum = 0;
  int n = 0;
  for (int i = 0; i < kSamples; i++) {
    int mv = 0;
    if (adc_oneshot_get_calibrated_result(adc, cali, channel, &mv) == ESP_OK) {
      sum += mv;
      n++;
    }
  }
  divider_connect(false);
  if (n == 0) return 0;
  const float pin_mv = static_cast<float>(sum) / n;
  return static_cast<uint32_t>(pin_mv * config.divider + 0.5f);
}

void battery_task(void *) {
  uint32_t filtered = 0;
  TickType_t last_log = 0;
  int below_cutoff = 0;
  bool critical_reported = false;
  for (;;) {
    const uint32_t mv = measure_mv();
    const bool plausible = fy_battery_plausible(mv);
    // Smooth load-dependent ripple; restart the filter after an implausible
    // reading so a newly connected battery reads correctly at once.
    filtered = (plausible && filtered) ? (filtered * 3 + mv) / 4 : (plausible ? mv : 0);
    const uint8_t pct = plausible ? fy::battery::percent_from_mv(filtered, CONFIG_FY_BATTERY_EMPTY_MV,
                                                                 CONFIG_FY_BATTERY_FULL_MV)
                                  : 0;

    // Below the cutoff for long enough: protect the cell.
    below_cutoff = (plausible && filtered < CONFIG_FY_BATTERY_CUTOFF_MV) ? below_cutoff + 1 : 0;
    const bool critical = below_cutoff >= kCriticalSamples;

    portENTER_CRITICAL(&lock);
    status.valid = plausible;
    status.mv = static_cast<uint16_t>(filtered);
    status.raw_mv = static_cast<uint16_t>(mv > 0xffff ? 0xffff : mv);
    status.percent = pct;
    status.critical = critical;
    portEXIT_CRITICAL(&lock);

    if (critical && !critical_reported) {
      critical_reported = true;
      ESP_LOGW(TAG, "battery %lu mV below cutoff %d mV for %d s: shutting down",
               static_cast<unsigned long>(filtered), CONFIG_FY_BATTERY_CUTOFF_MV,
               static_cast<int>(kCriticalSamples * kIntervalMs / 1000));
      if (critical_handler) critical_handler(static_cast<uint16_t>(filtered));
    }

    const TickType_t now = xTaskGetTickCount();
    if (last_log == 0 || now - last_log >= pdMS_TO_TICKS(kLogIntervalMs)) {
      last_log = now ? now : 1;
      if (plausible) {
        ESP_LOGI(TAG, "status mv=%lu raw_mv=%lu percent=%u (empty=%d full=%d cutoff=%d)",
                 static_cast<unsigned long>(filtered), static_cast<unsigned long>(mv), pct,
                 CONFIG_FY_BATTERY_EMPTY_MV, CONFIG_FY_BATTERY_FULL_MV,
                 CONFIG_FY_BATTERY_CUTOFF_MV);
      } else {
        ESP_LOGI(TAG, "status no battery reading (raw_mv=%lu)", static_cast<unsigned long>(mv));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
  }
}

}  // namespace

bool fy_battery_plausible(uint32_t mv) { return mv >= kMinPlausibleMv && mv <= kMaxPlausibleMv; }

esp_err_t fy_battery_measure_once(uint16_t *mv) {
  ESP_RETURN_ON_ERROR(ensure_adc(), TAG, "battery measurement unavailable");
  const uint32_t v = measure_mv();
  *mv = static_cast<uint16_t>(v > 0xffff ? 0xffff : v);
  return ESP_OK;
}

void fy_battery_set_critical_handler(fy_battery_critical_handler handler) {
  critical_handler = handler;
}

esp_err_t fy_battery_start() {
  const esp_err_t err = ensure_adc();
  if (err != ESP_OK) return err;
  portENTER_CRITICAL(&lock);
  status.available = true;
  portEXIT_CRITICAL(&lock);
  if (xTaskCreate(battery_task, "fy_battery", 3072, nullptr, 2, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "measuring on GPIO%d (divider %.3f), cutoff %d mV, start minimum %d mV",
           config.sense_gpio, config.divider, CONFIG_FY_BATTERY_CUTOFF_MV,
           CONFIG_FY_BATTERY_BOOT_MIN_MV);
  return ESP_OK;
}

void fy_battery_get_status(fy_battery_status *out) {
  portENTER_CRITICAL(&lock);
  *out = status;
  portEXIT_CRITICAL(&lock);
}
