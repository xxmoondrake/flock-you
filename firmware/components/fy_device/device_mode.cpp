// SPDX-License-Identifier: MIT

#include "fy_device_mode.h"

namespace fy::device {

Outputs outputs(const State &state) {
  if (state.dashboard == Dashboard::Wifi) return {false, false, true, false};
  const bool scanning = state.selected == Mode::Scanning;
  return {scanning, scanning, false, state.dashboard == Dashboard::Usb};
}

void select_mode(State &state, Mode mode) { state.selected = mode; }

void start_dashboard(State &state, Dashboard kind, uint32_t now_ms) {
  if (kind == Dashboard::Off) return;
  state.dashboard = kind;
  state.last_activity_ms = now_ms;
}

void stop_dashboard(State &state) { state.dashboard = Dashboard::Off; }

void note_activity(State &state, uint32_t now_ms) {
  if (state.dashboard == Dashboard::Wifi) state.last_activity_ms = now_ms;
}

uint32_t dashboard_remaining_ms(const State &state, uint32_t now_ms, uint32_t idle_limit_ms) {
  if (state.dashboard != Dashboard::Wifi) return 0;
  const uint32_t idle = now_ms - state.last_activity_ms;  // wraps correctly
  return idle >= idle_limit_ms ? 0 : idle_limit_ms - idle;
}

bool count_press(PressCounter &c, uint32_t now_ms, uint32_t held_ms) {
  if (held_ms > kActivationMaxHoldMs) {
    c.count = 0;
    return false;
  }
  if (c.count > 0 && now_ms - c.last_release_ms > kActivationMaxGapMs) c.count = 0;
  c.count++;
  c.last_release_ms = now_ms;
  if (c.count < kActivationPresses) return false;
  c.count = 0;
  return true;
}

}  // namespace fy::device
