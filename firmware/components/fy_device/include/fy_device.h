// SPDX-License-Identifier: MIT
//
// Device management: applies the selected mode (scanning or standby) and Wi-Fi
// dashboard sessions to the radios and the status LED.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "fy_device_mode.h"

// Starts the device task in Scanning with no dashboard session. Call after
// fy_detector_start().
esp_err_t fy_device_start();

// Selects scanning or standby. During a dashboard session the selection takes
// effect when the session ends.
void fy_device_select_mode(fy::device::Mode mode);

// Starts or stops a Wi-Fi dashboard session. Returns at once; the outcome
// shows in fy_device_get_status().
void fy_device_set_dashboard(bool on);

// Records user interaction (a button press or a dashboard page request),
// which keeps a dashboard session open.
void fy_device_note_activity();

struct fy_device_status {
  fy::device::Mode selected;
  bool dashboard;         // a session is running
  bool dashboard_failed;  // the last start attempt failed
  uint32_t dashboard_remaining_ms;
};

// Copies the current status. Safe from any task.
void fy_device_get_status(fy_device_status *out);
