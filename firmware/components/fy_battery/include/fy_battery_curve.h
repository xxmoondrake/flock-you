// SPDX-License-Identifier: MIT
//
// Battery voltage to state-of-charge estimate. Pure C++ with no ESP-IDF
// dependencies so it can be tested on a host computer.

#pragma once

#include <cstdint>

namespace fy::battery {

// Default endpoints of the reference curve: a typical single-cell Li-ion
// battery under light load.
constexpr uint32_t kReferenceFullMv = 4200;
constexpr uint32_t kReferenceEmptyMv = 3300;

// Estimates the charge percentage (0-100) from a battery voltage.
//
// The reference Li-ion discharge curve is stretched linearly so that
// `full_mv` maps to 100% and `empty_mv` to 0%; tuning those two values to a
// particular battery and divider calibrates the estimate. Voltage is only a
// rough proxy for charge: load, temperature, age, and charging all shift it.
uint8_t percent_from_mv(uint32_t mv, uint32_t empty_mv = kReferenceEmptyMv,
                        uint32_t full_mv = kReferenceFullMv);

}  // namespace fy::battery
