// SPDX-License-Identifier: MIT
//
// Status LED: shows the operating mode and flashes on specific matches.

#pragma once

#include "esp_err.h"
#include "fy_status_pattern.h"

// Starts driving the board's LED, solid in the booting mode until
// fy_status_led_set_mode() moves it on. Call after fy_board_init().
// Returns ESP_ERR_NOT_SUPPORTED when the board has no LED.
esp_err_t fy_status_led_start();

// Switches the LED to the pattern for `mode`. Mode patterns are shown when
// CONFIG_FY_STATUS_LED_PATTERNS is set; otherwise the LED stays dark between
// match flashes.
void fy_status_led_set_mode(fy::status::Mode mode);

// Shows the match burst over the current pattern. Callable from any task;
// does nothing before fy_status_led_start().
void fy_status_led_indicate_match();
