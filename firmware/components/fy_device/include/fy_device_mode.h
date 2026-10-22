// SPDX-License-Identifier: MIT
//
// Device modes: which radios detect, and when a Wi-Fi dashboard session ends.
// Pure C++ with no ESP-IDF dependencies so it can be tested on a host
// computer; the device task applies the outputs.

#pragma once

#include <cstdint>

namespace fy::device {

// The mode the user selects. Every boot starts in Scanning.
enum class Mode : uint8_t {
  Scanning = 0,  // Wi-Fi and BLE detection
  Standby,       // on, but not scanning
};

struct State {
  Mode selected = Mode::Scanning;
  bool dashboard = false;         // a Wi-Fi dashboard session is running
  uint32_t last_activity_ms = 0;  // last user interaction during a session
};

// What the radios should do.
struct Outputs {
  bool wifi_capture;  // promiscuous Wi-Fi detection
  bool ble_scan;      // passive BLE detection
  bool access_point;  // the dashboard's access point
};

// Detection runs only in Scanning without a dashboard session. A session
// pauses both Wi-Fi and BLE detection: the Wi-Fi radio serves the access
// point, and BLE stays off until coexistence is shown to be stable.
Outputs outputs(const State &state);

// A selection made during a session takes effect when the session ends.
void select_mode(State &state, Mode mode);
void start_dashboard(State &state, uint32_t now_ms);
void stop_dashboard(State &state);
void note_activity(State &state, uint32_t now_ms);

// Time left before an inactive session ends; 0 without a session or once it
// has expired.
uint32_t dashboard_remaining_ms(const State &state, uint32_t now_ms, uint32_t idle_limit_ms);

// On boards without a display (the XIAO), five deliberate short presses of
// the button start or stop the Wi-Fi dashboard.
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
