// SPDX-License-Identifier: MIT
//
// Device management: applies the selected mode (scanning or standby) and
// dashboard sessions (Wi-Fi or USB) to the radios, USB port, and status LED.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "fy_device_mode.h"

// Starts the device task in Scanning with no dashboard session. Call after
// fy_detector_start().
esp_err_t fy_device_start();

// Selects scanning or standby. During a Wi-Fi dashboard session the selection
// takes effect when the session ends.
void fy_device_select_mode(fy::device::Mode mode);

// Starts a dashboard session of `kind`, or stops the running one. Return at
// once; the outcome shows in fy_device_get_status().
void fy_device_start_dashboard(fy::device::Dashboard kind);
void fy_device_stop_dashboard();

// Records user interaction (a button press or a dashboard page request),
// which keeps a Wi-Fi dashboard session open.
void fy_device_note_activity();

struct fy_device_status {
  fy::device::Mode selected;
  fy::device::Dashboard dashboard;  // the running session, if any
  bool dashboard_failed;            // the last start attempt failed
  uint32_t dashboard_remaining_ms;  // Wi-Fi sessions only
};

// Copies the current status. Safe from any task.
void fy_device_get_status(fy_device_status *out);
