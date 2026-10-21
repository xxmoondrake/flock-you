// SPDX-License-Identifier: MIT
//
// GNSS receiver driver: reads NMEA from the board's GPS UART, sends the
// receiver's setup commands where the board provides a TX pin, and keeps the
// latest navigation data with its age.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "fy_nmea.h"

// A position older than this is stale and must not be presented as current.
constexpr uint32_t kFyGpsPositionStaleMs = 5000;

struct fy_gps_status {
  bool available;         // the board has a configured receiver
  bool receiving;         // NMEA arrived within the last few seconds
  fy::gps::NavData nav;   // last decoded data; validity flags as last reported
  bool position_current;  // nav.position_valid and not stale
  uint32_t position_age_ms;  // since the last valid position (UINT32_MAX if never)
  uint32_t time_age_ms;      // since the last valid time (UINT32_MAX if never)
  uint32_t sentences;
  uint32_t checksum_errors;
};

// Starts the receiver task. Returns ESP_ERR_NOT_SUPPORTED when the board has
// no usable GPS (absent, or wiring not yet confirmed).
esp_err_t fy_gps_start();

// Copies the current status. Safe from any task.
void fy_gps_get_status(fy_gps_status *out);
