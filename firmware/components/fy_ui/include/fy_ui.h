// SPDX-License-Identifier: MIT
//
// Screens and button navigation for boards with a display.

#pragma once

#include <cstdint>

#include "esp_err.h"

// Starts refreshing the board's display and handling its button.
// Returns ESP_ERR_NOT_SUPPORTED when the board has no display.
esp_err_t fy_ui_start();

// Draws the low-battery notice and holds it for `duration_ms`, without the
// UI task (for the start-up check). Does nothing without a display.
void fy_ui_show_low_battery_blocking(uint16_t mv, bool starting, uint32_t duration_ms);

// Asks the running UI task to show the low-battery notice for 3 s, save open
// encounters, and power off with a timed re-check. Returns false when no UI
// task is running, in which case the caller must power off itself.
bool fy_ui_request_low_battery_power_off(uint16_t mv);
