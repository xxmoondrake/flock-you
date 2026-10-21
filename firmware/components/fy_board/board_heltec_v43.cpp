// SPDX-License-Identifier: MIT
//
// Heltec WiFi LoRa 32 V4.3, standard V4 variant (ESP32-S3R2, 16 MB flash,
// native USB).
//
// Pin sources: Heltec's "Wi-Fi LoRa 32 V4 Pin Map" (V4.3.1 datasheet), the
// Meshtastic heltec_v4 variant, and the flocker DEFLOCK 3.2.0 firmware, whose
// OLED setup uses these pins:
//   OLED    SSD1306 128x64 at I2C 0x3C; SDA GPIO17, SCL GPIO18, reset GPIO21
//   Vext    GPIO36, active low; powers the OLED (and the LoRa antenna boost)
//   LED     GPIO35, active high
//   Button  PRG on GPIO0 (boot strapping pin); not used yet
//   GNSS    Quectel L76K on the 8-pin GNSS connector. Power (VGNSS_Ctrl)
//           GPIO34, active low. Module TX -> GPIO39 (ESP32 RX), 9600 baud.
//           ESP32 TX -> GPIO38 (module RX) for configuration commands; with
//           the module's output on GPIO39, GPIO38 can only be its input.
//           Wake (GNSS_Wakeup) GPIO40 is held high to keep the
//           receiver out of standby, and reset (GNSS_RST, active low) GPIO42
//           is held high, both as Meshtastic does. PPS GPIO41 is left
//           undriven.
//   Battery VBAT_Read on GPIO1 (ADC1) through a divider; ADC_Ctrl GPIO37,
//           active high, connects the divider and is driven only while
//           measuring. Attenuation 2.5 dB and ratio 4.9 x 1.045, as in
//           Meshtastic's heltec_v4 variant. GPIO37 also reaches the GNSS
//           connector (labelled VEXT_CTRL there); the L76K does not need it.

#include "sdkconfig.h"

#if CONFIG_FY_BOARD_HELTEC_V43

#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fy_board.h"

namespace {

const char *TAG = "board";

constexpr gpio_num_t kVextPin = GPIO_NUM_36;  // active low
constexpr gpio_num_t kLedPin = GPIO_NUM_35;   // active high
constexpr gpio_num_t kOledSda = GPIO_NUM_17;
constexpr gpio_num_t kOledScl = GPIO_NUM_18;
constexpr gpio_num_t kOledReset = GPIO_NUM_21;
constexpr uint16_t kOledAddress = 0x3c;
constexpr uint32_t kI2cHz = 400000;
constexpr gpio_num_t kGnssPowerPin = GPIO_NUM_34;  // active low
constexpr gpio_num_t kGnssWakePin = GPIO_NUM_40;   // high keeps the receiver awake
constexpr gpio_num_t kGnssResetPin = GPIO_NUM_42;  // active low
constexpr int kGnssRxPin = 39;
constexpr int kGnssTxPin = 38;
constexpr int kGnssBaud = 9600;  // L76K default
constexpr uint32_t kGnssWarmupMs = 1000;  // as Meshtastic's PERIPHERAL_WARMUP_MS
constexpr int kBatterySensePin = 1;
constexpr int kBatteryEnablePin = 37;     // ADC_Ctrl, active high
constexpr int kBatteryAdcAtten = 1;       // ADC_ATTEN_DB_2_5
constexpr float kBatteryDivider = 4.9f * 1.045f;
constexpr gpio_num_t kButtonPin = GPIO_NUM_0;  // PRG, active low

bool led_ready = false;
i2c_master_bus_handle_t i2c_bus = nullptr;
i2c_master_dev_handle_t oled = nullptr;
bool display_ok = false;

void output(gpio_num_t pin, int level) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  gpio_set_level(pin, level);
}

void led_write(bool on) { gpio_set_level(kLedPin, on ? 1 : 0); }

// Control byte 0x00: the rest of the transfer is command bytes.
esp_err_t oled_commands(const uint8_t *cmds, size_t n) {
  uint8_t buf[32];
  if (n + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
  buf[0] = 0x00;
  memcpy(buf + 1, cmds, n);
  return i2c_master_transmit(oled, buf, n + 1, 100);
}

bool oled_init() {
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = kOledSda;
  bus_cfg.scl_io_num = kOledScl;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&bus_cfg, &i2c_bus) != ESP_OK) return false;

  if (i2c_master_probe(i2c_bus, kOledAddress, 100) != ESP_OK) {
    ESP_LOGE(TAG, "no OLED acknowledged at 0x%02x", kOledAddress);
    return false;
  }

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = kOledAddress;
  dev_cfg.scl_speed_hz = kI2cHz;
  if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &oled) != ESP_OK) return false;

  // SSD1306 128x64 with the internal charge pump, horizontal addressing,
  // and the panel orientation of the Heltec V4 OLED.
  static const uint8_t kInit[] = {
      0xae,        // display off
      0xd5, 0x80,  // clock divide
      0xa8, 0x3f,  // multiplex 64
      0xd3, 0x00,  // display offset 0
      0x40,        // start line 0
      0x8d, 0x14,  // charge pump on
      0x20, 0x00,  // horizontal addressing
      0xa1,        // segment remap
      0xc8,        // COM scan descending
      0xda, 0x12,  // COM pins, alternative configuration
      0x81, 0xcf,  // contrast
      0xd9, 0xf1,  // pre-charge period
      0xdb, 0x40,  // VCOMH deselect level
      0xa4,        // display follows RAM
      0xa6,        // normal (not inverted)
      0x2e,        // scrolling off
  };
  if (oled_commands(kInit, sizeof(kInit)) != ESP_OK) return false;

  static const uint8_t kBlank[1024] = {};
  display_ok = true;
  fy_board_display_flush(kBlank);
  static const uint8_t kOn[] = {0xaf};
  return oled_commands(kOn, sizeof(kOn)) == ESP_OK;
}

}  // namespace

void fy_board_init() {
  // Release pin holds left by a previous power-off.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(kVextPin);
  gpio_hold_dis(kGnssPowerPin);

  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << kButtonPin;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&button));

  output(kLedPin, 0);
  led_ready = true;

  // Power the OLED and the GNSS receiver, then let the rails settle.
  output(kVextPin, 0);
  output(kGnssResetPin, 1);  // released before power-up
  output(kGnssWakePin, 1);
  output(kGnssPowerPin, 0);
  vTaskDelay(pdMS_TO_TICKS(100));

  output(kOledReset, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(kOledReset, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  display_ok = oled_init();
  if (!display_ok) ESP_LOGE(TAG, "OLED initialization failed; continuing without display");

  // Three short blinks confirm the LED pin at boot.
  for (int i = 0; i < 3; i++) {
    led_write(true);
    vTaskDelay(pdMS_TO_TICKS(80));
    led_write(false);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  ESP_LOGI(TAG, "Heltec V4.3: display=%s, GNSS powered (RX GPIO%d)", display_ok ? "ok" : "absent",
           kGnssRxPin);
}

const char *fy_board_name() { return "heltec_v43"; }

bool fy_board_has_led() { return true; }

void fy_board_led_set(bool on) {
  if (led_ready) led_write(on);
}

bool fy_board_display_available() { return display_ok; }

void fy_board_display_flush(const uint8_t *frame) {
  if (!display_ok || !frame) return;
  static const uint8_t kWindow[] = {0x21, 0, 127, 0x22, 0, 7};  // full column and page range
  if (oled_commands(kWindow, sizeof(kWindow)) != ESP_OK) return;
  static uint8_t buf[1 + 1024];
  buf[0] = 0x40;  // control byte: display data follows
  memcpy(buf + 1, frame, 1024);
  i2c_master_transmit(oled, buf, sizeof(buf), 200);
}

bool fy_board_gps_config_get(fy_board_gps_config *out) {
  out->uart_num = 1;
  out->rx_pin = kGnssRxPin;
  out->tx_pin = kGnssTxPin;
  out->baud = kGnssBaud;
  out->model = fy_gps_receiver_model::quectel_l76k;
  out->warmup_ms = kGnssWarmupMs;
  return true;
}

bool fy_board_has_button() { return true; }

bool fy_board_button_pressed() { return gpio_get_level(kButtonPin) == 0; }

void fy_board_power_off(uint32_t timer_wake_s) {
  ESP_LOGW(TAG, "powering off; press PRG to wake%s", timer_wake_s ? " (timed re-check set)" : "");
  if (display_ok) {
    static const uint8_t kSleep[] = {0xae, 0x8d, 0x10};  // display off, charge pump off
    oled_commands(kSleep, sizeof(kSleep));
  }
  // Pins are configured here too, since this may run before fy_board_init().
  output(kLedPin, 0);
  // Cut OLED and GNSS power and hold those levels through deep sleep.
  gpio_hold_dis(kGnssPowerPin);
  gpio_hold_dis(kVextPin);
  output(kGnssPowerPin, 1);
  output(kVextPin, 1);
  gpio_hold_en(kGnssPowerPin);
  gpio_hold_en(kVextPin);
  gpio_deep_sleep_hold_en();
  // Wait for the button to be released so it does not wake the board at once.
  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << kButtonPin;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&button);
  while (fy_board_button_pressed()) vTaskDelay(pdMS_TO_TICKS(20));
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_sleep_enable_ext0_wakeup(kButtonPin, 0);
  if (timer_wake_s) esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_wake_s) * 1000000ULL);
  esp_deep_sleep_start();
}

bool fy_board_battery_config_get(fy_board_battery_config *out) {
  out->sense_gpio = kBatterySensePin;
  out->enable_gpio = kBatteryEnablePin;
  out->enable_level = 1;
  out->adc_atten = kBatteryAdcAtten;
  out->divider = kBatteryDivider;
  return true;
}

#endif  // CONFIG_FY_BOARD_HELTEC_V43
