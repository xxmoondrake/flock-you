// SPDX-License-Identifier: MIT
//
// OLED screen layouts. Pure C++ (host-testable): the caller gathers the state
// into a UiModel and these draw it.

#pragma once

#include <cstdint>

#include "fy_canvas.h"
#include "fy_ui_controller.h"

namespace fy::ui {

struct DetectionRow {
  const char *name;  // device type name, at most 12 characters
  bool possible;
  uint16_t session;  // distinct devices this session
  uint16_t stored;   // distinct devices in stored history
};

constexpr int kMaxDetectionRows = 24;

struct UiModel {
  uint32_t now_ms;

  bool wifi_capture;
  bool ble_scan;
  uint8_t channel;

  // Device management: the selected mode and the Wi-Fi dashboard session.
  bool standby;  // standby selected (not scanning)
  bool dashboard_active;  // either dashboard
  bool dashboard_usb;     // the USB dashboard (detection keeps running)
  bool dashboard_failed;  // the last start attempt failed
  uint32_t dashboard_left_ms;
  char dashboard_ssid[33];
  char dashboard_passkey[64];  // empty once the passkey has been changed
  char dashboard_address[16];
  uint8_t dashboard_clients;

  bool gps_available;
  bool gps_receiving;
  bool gps_time_valid;
  uint8_t utc_hour, utc_minute, utc_second;
  uint32_t gps_time_age_ms;
  bool gps_position_current;
  bool gps_position_ever;
  uint32_t gps_position_age_ms;
  double latitude_deg, longitude_deg;
  uint8_t fix_type;  // 0 unknown, 1 none, 2 2D, 3 3D
  uint8_t satellites;  // used in the fix
  bool hdop_valid;
  float hdop;
  // Receiver diagnostics, shown while there is no current fix.
  uint8_t sats_in_view;
  uint8_t sats_with_signal;
  uint8_t antenna;  // 0 unknown, 1 ok, 2 open, 3 short

  uint32_t reported;
  uint32_t reported_specific;
  bool last_valid;
  const char *last_rule;
  bool last_specific;
  int8_t last_rssi;
  uint32_t last_at_ms;

  bool alert_active;      // a specific match was reported moments ago
  uint32_t alert_age_ms;

  bool battery_available;  // the board can measure a battery
  bool battery_valid;      // a plausible reading exists
  uint16_t battery_mv;
  uint8_t battery_percent;

  bool storage_warning;  // record storage is nearly full; offload soon
  uint8_t storage_percent;

  // Detections screen: device types with at least one device, in display
  // order. Counts are lower bounds when *_saturated is set; while
  // stored_pending is set the stored counts are the last complete tally,
  // being recounted after old records left the log.
  DetectionRow detections[kMaxDetectionRows];
  int detection_rows;
  bool counts_saturated;
  bool stored_pending;

  // Data screen.
  bool storage_available;
  uint32_t records_used;
  uint32_t records_capacity;
  uint32_t encounter_records;
  uint32_t battery_records;
  uint32_t oldest_utc;  // Unix seconds; 0 = unknown
  uint32_t boot;

  bool erasing;
  uint8_t erase_percent;
  bool power_off_armed;  // button held long enough; releasing powers off
};

// Clears the canvas and draws the screen, menu, or prompt for `state`.
void render(display::Canvas &canvas, const UiModel &m, const UiState &state);

// Low-battery notice shown for a few seconds before powering off. `mv` is
// the measured voltage; `starting` is true when the board refused to start.
void render_low_battery(display::Canvas &canvas, uint16_t mv, bool starting);

// The individual screens, for tests and previews.
void render_home(display::Canvas &canvas, const UiModel &m);
void render_detections(display::Canvas &canvas, const UiModel &m);
void render_data(display::Canvas &canvas, const UiModel &m);
void render_device(display::Canvas &canvas, const UiModel &m);

// Formats an elapsed time compactly: "12s", "4m", "3h", "2d".
void format_age(uint32_t ms, char *out, int cap);

}  // namespace fy::ui
