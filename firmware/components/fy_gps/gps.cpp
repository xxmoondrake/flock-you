// SPDX-License-Identifier: MIT

#include "fy_gps.h"

#include <climits>
#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fy_board.h"
#include "sdkconfig.h"

namespace {

const char *TAG = "gps";

#if CONFIG_FY_GPS_LOG_RAW_NMEA
// Diagnostics: collects received bytes into lines and logs each one, with
// anything outside printable ASCII replaced.
class RawEcho {
 public:
  void feed(uint8_t c) {
    if (c == '\r' || c == '\n') {
      if (len_ > 0) {
        line_[len_] = '\0';
        ESP_LOGI("nmea", "%s", line_);
        len_ = 0;
      }
      return;
    }
    if (len_ < sizeof(line_) - 1) line_[len_++] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
  }

 private:
  char line_[128];
  size_t len_ = 0;
};
#endif

// No NMEA for this long means the receiver is silent (unpowered, unplugged,
// or wrong pin).
constexpr uint32_t kSilentMs = 3000;
constexpr uint32_t kStatusIntervalMs = 30000;

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
bool started = false;
uart_port_t uart = UART_NUM_1;
fy_board_gps_config config = {};

// Guarded by `lock`.
fy::gps::NavData nav;
bool ever_position = false;
bool ever_time = false;
bool ever_sentence = false;
uint32_t last_sentence_ms = 0;
uint32_t last_position_ms = 0;
uint32_t last_time_ms = 0;
uint32_t sentence_count = 0;
uint32_t checksum_error_count = 0;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void log_transitions(const fy_gps_status &s, bool &was_receiving, bool &had_position,
                     bool &had_time) {
  if (s.receiving != was_receiving) {
    if (s.receiving) {
      ESP_LOGI(TAG, "receiving NMEA");
    } else {
      ESP_LOGW(TAG, "no NMEA for %lu ms", static_cast<unsigned long>(kSilentMs));
    }
    was_receiving = s.receiving;
  }
  if (s.nav.time_valid != had_time) {
    if (s.nav.time_valid) {
      ESP_LOGI(TAG, "time valid: %04u-%02u-%02uT%02u:%02u:%02uZ", s.nav.utc.year, s.nav.utc.month,
               s.nav.utc.day, s.nav.utc.hour, s.nav.utc.minute, s.nav.utc.second);
    } else {
      ESP_LOGW(TAG, "time lost");
    }
    had_time = s.nav.time_valid;
  }
  if (s.position_current != had_position) {
    if (s.position_current) {
      ESP_LOGI(TAG, "fix acquired: type=%u quality=%u sats=%u hdop=%.1f", s.nav.fix_type,
               s.nav.fix_quality, s.nav.satellites, s.nav.hdop_valid ? s.nav.hdop : 0.0f);
    } else {
      ESP_LOGW(TAG, "fix lost or stale");
    }
    had_position = s.position_current;
  }
}

const char *antenna_name(fy::gps::Antenna a) {
  switch (a) {
    case fy::gps::Antenna::Ok:
      return "ok";
    case fy::gps::Antenna::Open:
      return "open";
    case fy::gps::Antenna::Short:
      return "short";
    default:
      return "unknown";
  }
}

void log_status(const fy_gps_status &s) {
  ESP_LOGI(TAG,
           "status receiving=%d sentences=%lu checksum_err=%lu time=%s fix=%s type=%u quality=%u "
           "sats_used=%u in_view=%u with_signal=%u max_cn0=%u antenna=%s hdop=%.1f "
           "receiver=\"%s\"",
           s.receiving ? 1 : 0, static_cast<unsigned long>(s.sentences),
           static_cast<unsigned long>(s.checksum_errors), s.nav.time_valid ? "valid" : "none",
           s.position_current ? "current" : (s.position_age_ms != UINT32_MAX ? "stale" : "none"),
           s.nav.fix_type, s.nav.fix_quality, s.nav.satellites, s.nav.satellites_in_view,
           s.nav.satellites_with_signal, s.nav.max_cn0, antenna_name(s.nav.antenna),
           s.nav.hdop_valid ? s.nav.hdop : 0.0f, s.nav.receiver_info);
}

// Sends one NMEA/PCAS command, adding the checksum and line ending.
void send_command(const char *body) {
  uint8_t sum = 0;
  for (const char *p = body; *p; p++) sum ^= static_cast<uint8_t>(*p);
  char line[96];
  const int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
  if (n > 0 && n < static_cast<int>(sizeof(line))) uart_write_bytes(uart, line, n);
}

// Quectel L76K setup, following Meshtastic's L76K handling: query the
// firmware version (answered with "$GPTXT,...,SW=..." text) and enable GPS,
// BeiDou, and GLONASS. Output sentences are left at the receiver's defaults,
// which include the GSV and TXT sentences used for diagnostics. The settings
// are not saved to the receiver.
void configure_l76k() {
  send_command("PCAS06,0");
  vTaskDelay(pdMS_TO_TICKS(250));
  send_command("PCAS04,7");
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_LOGI(TAG, "configured L76K: GPS + BeiDou + GLONASS");
}

void gps_task(void *) {
  fy::gps::NmeaParser parser;
  uint8_t buf[128];
  bool was_receiving = false, had_position = false, had_time = false;
  const uint32_t started_ms = now_ms();
  uint32_t last_status_ms = started_ms;
  bool configured = config.tx_pin < 0 || config.model == fy_gps_receiver_model::generic_nmea;
  char logged_info[sizeof(fy::gps::NavData::receiver_info)] = {};
#if CONFIG_FY_GPS_LOG_RAW_NMEA
  RawEcho echo;
#endif
  for (;;) {
    const int n = uart_read_bytes(uart, buf, sizeof(buf), pdMS_TO_TICKS(200));
    const uint32_t now = now_ms();
    for (int i = 0; i < n; i++) {
#if CONFIG_FY_GPS_LOG_RAW_NMEA
      echo.feed(buf[i]);
#endif
      const uint8_t updated = parser.feed(static_cast<char>(buf[i]));
      if (!updated) continue;
      const fy::gps::NavData &d = parser.data();
      portENTER_CRITICAL(&lock);
      nav = d;
      ever_sentence = true;
      last_sentence_ms = now;
      if ((updated & fy::gps::kUpdatedPosition) && d.position_valid) {
        ever_position = true;
        last_position_ms = now;
      }
      if ((updated & fy::gps::kUpdatedTime) && d.time_valid) {
        ever_time = true;
        last_time_ms = now;
      }
      portEXIT_CRITICAL(&lock);
    }
    portENTER_CRITICAL(&lock);
    sentence_count = parser.sentences();
    checksum_error_count = parser.checksum_errors();
    portEXIT_CRITICAL(&lock);

    fy_gps_status s;
    fy_gps_get_status(&s);
    log_transitions(s, was_receiving, had_position, had_time);
    // Configure once the receiver is talking and has had its warm-up time.
    if (!configured && s.receiving && now - started_ms >= config.warmup_ms) {
      configure_l76k();
      configured = true;
    }
    if (strcmp(s.nav.receiver_info, logged_info) != 0) {
      memcpy(logged_info, s.nav.receiver_info, sizeof(logged_info));
      ESP_LOGI(TAG, "receiver reports: %s", logged_info);
    }
    if (now - last_status_ms >= kStatusIntervalMs) {
      last_status_ms = now;
      log_status(s);
    }
  }
}

}  // namespace

esp_err_t fy_gps_start() {
  fy_board_gps_config cfg;
  if (!fy_board_gps_config_get(&cfg)) {
    ESP_LOGI(TAG, "no GPS receiver configured for this board");
    return ESP_ERR_NOT_SUPPORTED;
  }
  config = cfg;
  uart = static_cast<uart_port_t>(cfg.uart_num);

  uart_config_t uc = {};
  uc.baud_rate = cfg.baud;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  ESP_RETURN_ON_ERROR(uart_driver_install(uart, 2048, 0, 0, nullptr, 0), TAG, "uart install");
  ESP_RETURN_ON_ERROR(uart_param_config(uart, &uc), TAG, "uart config");
  // The TX pin is connected only when the board confirms where the
  // receiver's input is; otherwise the driver is receive only.
  const int tx = cfg.tx_pin >= 0 ? cfg.tx_pin : UART_PIN_NO_CHANGE;
  ESP_RETURN_ON_ERROR(uart_set_pin(uart, tx, cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                      TAG, "uart pins");

  if (xTaskCreate(gps_task, "fy_gps", 4096, nullptr, 3, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  started = true;
  ESP_LOGI(TAG, "UART%d RX GPIO%d TX GPIO%d at %d baud", cfg.uart_num, cfg.rx_pin, cfg.tx_pin,
           cfg.baud);
  return ESP_OK;
}

void fy_gps_get_status(fy_gps_status *out) {
  const uint32_t now = now_ms();
  portENTER_CRITICAL(&lock);
  out->available = started;
  out->nav = nav;
  out->receiving = ever_sentence && (now - last_sentence_ms) < kSilentMs;
  out->position_age_ms = ever_position ? now - last_position_ms : UINT32_MAX;
  out->time_age_ms = ever_time ? now - last_time_ms : UINT32_MAX;
  out->sentences = sentence_count;
  out->checksum_errors = checksum_error_count;
  portEXIT_CRITICAL(&lock);
  out->position_current = out->nav.position_valid && out->position_age_ms < kFyGpsPositionStaleMs;
}
