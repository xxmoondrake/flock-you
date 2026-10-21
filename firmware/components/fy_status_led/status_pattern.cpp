// SPDX-License-Identifier: MIT

#include "fy_status_pattern.h"

#include <cstddef>

namespace fy::status {
namespace {

struct Step {
  bool on;
  uint16_t ms;
};

// Short blinks read clearly on the XIAO's small LED without lighting a room.
constexpr Step kScanning[] = {{true, 100}, {false, 2900}};
constexpr Step kStandby[] = {{true, 100}, {false, 9900}};
constexpr Step kDashboard[] = {{true, 100}, {false, 150}, {true, 100}, {false, 2650}};
constexpr Step kMatch[] = {{true, 60}, {false, 80}, {true, 60}, {false, 80}, {true, 60}};

template <size_t N>
constexpr uint32_t total_ms(const Step (&steps)[N]) {
  uint32_t ms = 0;
  for (const Step &s : steps) ms += s.ms;
  return ms;
}

static_assert(total_ms(kDashboard) == total_ms(kScanning), "dashboard keeps the scanning cadence");
static_assert(total_ms(kMatch) == kMatchBurstMs, "kMatchBurstMs must match the burst steps");

// Position within one pass of `steps`; `t` must be less than their total.
template <size_t N>
LedState step_at(const Step (&steps)[N], uint32_t t) {
  for (const Step &s : steps) {
    if (t < s.ms) return {s.on, s.ms - t};
    t -= s.ms;
  }
  return {false, 0};
}

template <size_t N>
LedState repeating(const Step (&steps)[N], uint32_t elapsed_ms) {
  return step_at(steps, elapsed_ms % total_ms(steps));
}

}  // namespace

LedState mode_state(Mode mode, uint32_t elapsed_ms) {
  switch (mode) {
    case Mode::Booting:
      return {true, 0};
    case Mode::Scanning:
      return repeating(kScanning, elapsed_ms);
    case Mode::Standby:
      return repeating(kStandby, elapsed_ms);
    case Mode::Dashboard:
      return repeating(kDashboard, elapsed_ms);
  }
  return {false, 0};
}

LedState match_state(uint32_t elapsed_ms) {
  if (elapsed_ms >= kMatchBurstMs) return {false, 0};
  return step_at(kMatch, elapsed_ms);
}

}  // namespace fy::status
