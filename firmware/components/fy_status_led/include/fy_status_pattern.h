// SPDX-License-Identifier: MIT
//
// Status LED blink patterns. Pure C++ with no ESP-IDF dependencies so it can
// be tested on a host computer.

#pragma once

#include <cstdint>

namespace fy::status {

// What the board is doing, as shown on its status LED.
enum class Mode : uint8_t {
  Booting,    // solid until scanning starts
  Scanning,   // one short blink every 3 s
  Standby,    // one short blink every 10 s
  Dashboard,  // a double blink every 3 s, the scanning cadence
};

// The LED level at a moment in a pattern, and how long it stays that way.
struct LedState {
  bool on;
  uint32_t hold_ms;  // 0: unchanged until the mode changes
};

// Where the mode's pattern stands `elapsed_ms` after the mode began.
LedState mode_state(Mode mode, uint32_t elapsed_ms);

// Length of the quick triple flash shown for a specific match, which
// overrides the mode's pattern while it runs.
constexpr uint32_t kMatchBurstMs = 340;

// Where the match burst stands `elapsed_ms` after it began; off with no
// further change once the burst is over.
LedState match_state(uint32_t elapsed_ms);

}  // namespace fy::status
