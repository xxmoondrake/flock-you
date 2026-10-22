// SPDX-License-Identifier: MIT
//
// Board profile interface. Exactly one implementation is compiled, selected
// by CONFIG_FY_BOARD_* in sdkconfig. Shared code must not touch GPIO directly,
// and a board profile drives only pins confirmed for that board.

#pragma once

#include <cstdint>

// Configures the board's own peripherals and powers its display and GNSS
// receiver where present. Call once, before the radios start.
void fy_board_init();

// Short identifier for logs, e.g. "xiao_esp32s3".
const char *fy_board_name();

// ---- Status LED -------------------------------------------------------------

// True when the board has an LED the firmware may drive.
bool fy_board_has_led();
// Turns the LED on or off. Call after fy_board_init(), from one task only
// (the status LED task).
void fy_board_led_set(bool on);

// ---- Display ----------------------------------------------------------------

// True when a 128x64 monochrome display was found and initialized.
bool fy_board_display_available();

// Sends a full 1024-byte frame in SSD1306 page order (8 pages of 128 column
// bytes, bit 0 at the top). Call from one task only.
void fy_board_display_flush(const uint8_t *frame);

// ---- GNSS receiver ----------------------------------------------------------

enum class fy_gps_receiver_model { generic_nmea, quectel_l76k };

struct fy_board_gps_config {
  int uart_num;
  int rx_pin;  // ESP32 input carrying the receiver's NMEA output
  int tx_pin;  // ESP32 output to the receiver's input, or -1 for receive only
  int baud;
  fy_gps_receiver_model model;
  uint32_t warmup_ms;  // after power-up, before sending configuration
};

// Fills `out` and returns true when the board has a GNSS receiver whose
// wiring is confirmed; returns false otherwise.
bool fy_board_gps_config_get(fy_board_gps_config *out);

// ---- Battery ----------------------------------------------------------------

struct fy_board_battery_config {
  int sense_gpio;    // ADC input carrying the divided battery voltage
  int enable_gpio;   // divider enable output, or -1 if the divider is always connected
  int enable_level;  // level on enable_gpio that connects the divider
  int adc_atten;     // ADC_ATTEN_DB_* value suited to the divided voltage
  float divider;     // battery mV = sense-pin mV * divider
};

// Fills `out` and returns true when the board can measure a battery.
bool fy_board_battery_config_get(fy_board_battery_config *out);

// ---- User button and power ------------------------------------------------------

// True when the board has a user button: screen navigation on boards with a
// display, Wi-Fi dashboard activation (five quick presses) otherwise.
bool fy_board_has_button();
// True while the user button is held down.
bool fy_board_button_pressed();
// Turns off the display, GNSS, and LED and enters deep sleep. The user button
// wakes the board, which then restarts normally. A nonzero `timer_wake_s`
// also wakes it after that many seconds (used to re-check a low battery).
// Safe to call before fy_board_init().
[[noreturn]] void fy_board_power_off(uint32_t timer_wake_s = 0);
