// SPDX-License-Identifier: MIT
//
// Device modes: which radios detect, which dashboard runs, and when a Wi-Fi
// dashboard session ends. Pure C++ with no ESP-IDF dependencies so it can be
// tested on a host computer; the device task applies the outputs.

#pragma once

#include <cstdint>

namespace fy::device {

// The mode the user selects. Every boot starts in Scanning.
enum class Mode : uint8_t {
  Scanning = 0,  // Wi-Fi and BLE detection
  Standby,       // on, but not scanning
};

// A dashboard session started from the device.
enum class Dashboard : uint8_t {
  Off = 0,
  Wifi,  // access point; pauses detection and ends after inactivity
  Usb,   // USB network; detection keeps running in the selected mode
};

struct State {
  Mode selected = Mode::Scanning;
  Dashboard dashboard = Dashboard::Off;
  uint32_t last_activity_ms = 0;  // last user interaction during a Wi-Fi session
};

// What the radios and USB port should do.
struct Outputs {
  bool wifi_capture;   // promiscuous Wi-Fi detection
  bool ble_scan;       // passive BLE detection
  bool access_point;   // the Wi-Fi dashboard's access point
  bool usb_dashboard;  // the USB dashboard (network adapter and console)
};

// Detection follows the selected mode, except during a Wi-Fi dashboard
// session, which pauses both Wi-Fi and BLE detection: the Wi-Fi radio serves
// the access point, and BLE stays off until coexistence is shown to be
// stable. A USB dashboard does not use the radio and pauses nothing.
Outputs outputs(const State &state);

// During a Wi-Fi session a new selection takes effect when the session ends.
void select_mode(State &state, Mode mode);
// Starts a session of `kind` (not Off), replacing any running one.
void start_dashboard(State &state, Dashboard kind, uint32_t now_ms);
void stop_dashboard(State &state);
void note_activity(State &state, uint32_t now_ms);

// Time left before an inactive Wi-Fi session ends; 0 without one or once it
// has expired. USB sessions do not time out.
uint32_t dashboard_remaining_ms(const State &state, uint32_t now_ms, uint32_t idle_limit_ms);

// On boards without a display, five deliberate short presses of the button
// start or stop the Wi-Fi dashboard.
constexpr uint8_t kActivationPresses = 5;
constexpr uint32_t kActivationMaxHoldMs = 500;  // a longer hold is not counted
constexpr uint32_t kActivationMaxGapMs = 1000;  // a longer pause starts over

struct PressCounter {
  uint8_t count = 0;
  uint32_t last_release_ms = 0;
};

// Records a press released at `now_ms` after being held `held_ms`. Returns
// true on the fifth qualifying press in a row, then starts over.
bool count_press(PressCounter &counter, uint32_t now_ms, uint32_t held_ms);

}  // namespace fy::device
