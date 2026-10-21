// SPDX-License-Identifier: MIT

#include "fy_battery_curve.h"

namespace fy::battery {
namespace {

struct Point {
  uint16_t mv;
  uint8_t percent;
};

// Typical single-cell Li-ion discharge under light load, descending.
constexpr Point kCurve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
    {3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
    {3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3300, 0},
};
constexpr int kPoints = sizeof(kCurve) / sizeof(kCurve[0]);

}  // namespace

uint8_t percent_from_mv(uint32_t mv, uint32_t empty_mv, uint32_t full_mv) {
  if (full_mv <= empty_mv) return 0;
  if (mv >= full_mv) return 100;
  if (mv <= empty_mv) return 0;

  // Map the measured voltage onto the reference curve's voltage range.
  const uint32_t ref_mv =
      kReferenceEmptyMv +
      (mv - empty_mv) * (kReferenceFullMv - kReferenceEmptyMv) / (full_mv - empty_mv);

  for (int i = 1; i < kPoints; i++) {
    const Point &hi = kCurve[i - 1];
    const Point &lo = kCurve[i];
    if (ref_mv >= lo.mv) {
      // Linear interpolation between neighbouring curve points.
      const uint32_t span_mv = hi.mv - lo.mv;
      const uint32_t span_pct = hi.percent - lo.percent;
      return static_cast<uint8_t>(lo.percent + (ref_mv - lo.mv) * span_pct / span_mv);
    }
  }
  return 0;
}

}  // namespace fy::battery
