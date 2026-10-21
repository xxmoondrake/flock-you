// SPDX-License-Identifier: MIT
//
// Battery monitor: periodically measures the battery voltage through the
// board's divider, estimates the charge percentage, and reports when the
// battery stays below the protection cutoff.

#pragma once

#include <cstdint>

#include "esp_err.h"

struct fy_battery_status {
  bool available;   // the board can measure a battery
  bool valid;       // a plausible reading exists (a battery appears connected)
  uint16_t mv;      // smoothed battery voltage
  uint16_t raw_mv;  // latest sample before smoothing
  uint8_t percent;  // estimate from fy::battery::percent_from_mv
  bool critical;    // below CONFIG_FY_BATTERY_CUTOFF_MV long enough to shut down
};

// Starts periodic measurement. Returns ESP_ERR_NOT_SUPPORTED when the board
// has no battery measurement.
esp_err_t fy_battery_start();

// Copies the current status. Safe from any task.
void fy_battery_get_status(fy_battery_status *out);

// Measures the battery once, initializing the ADC if needed. Usable before
// the rest of the board is initialized. Returns ESP_ERR_NOT_SUPPORTED when
// the board has no battery measurement.
esp_err_t fy_battery_measure_once(uint16_t *mv);

// True when a reading looks like a connected single-cell battery rather than
// an absent one.
bool fy_battery_plausible(uint32_t mv);

// Called once, from the battery task, when the battery has stayed below the
// cutoff; the handler is expected to warn the user and power off.
using fy_battery_critical_handler = void (*)(uint16_t mv);
void fy_battery_set_critical_handler(fy_battery_critical_handler handler);
