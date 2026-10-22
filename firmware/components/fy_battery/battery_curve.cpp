// SPDX-License-Identifier: MIT

#include "fy_battery_curve.h"

namespace fy::battery {
namespace {

struct Point {
  uint16_t mv;
  uint8_t percent;
};

// Measured on a Heltec V4.3 with a 3000 mAh single cell (scanning, GPS, and
// OLED active), combined from two discharge runs, descending. Voltages are as
// the board's divider reads them, not at the cell terminals. The percentage
// is the share of runtime left before the 3200 mV cutoff; a full charge
// lasted about 20.5 hours.
constexpr Point kCurve[] = {
    {4270, 100}, {4204, 95}, {4158, 90}, {4096, 85}, {4034, 80}, {3996, 75}, {3970, 70},
    {3939, 65},  {3892, 60}, {3826, 55}, {3782, 50}, {3750, 45}, {3721, 40}, {3699, 35},
    {3680, 30},  {3648, 25}, {3612, 20}, {3572, 15}, {3518, 10}, {3471, 5},  {3451, 4},
    {3408, 3},   {3350, 2},  {3278, 1},  {3200, 0},
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
