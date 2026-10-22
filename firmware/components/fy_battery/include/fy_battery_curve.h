// SPDX-License-Identifier: MIT
//
// Battery voltage to state-of-charge estimate. Pure C++ with no ESP-IDF
// dependencies so it can be tested on a host computer.

#pragma once

#include <cstdint>

namespace fy::battery {

// Endpoints of the reference curve, measured on a Heltec V4.3 under normal
// load: the reading just after unplugging a full charge, and the protection
// cutoff.
constexpr uint32_t kReferenceFullMv = 4270;
constexpr uint32_t kReferenceEmptyMv = 3200;

// Estimates the charge percentage (0-100) from a battery voltage, as the
// share of runtime left before the cutoff.
//
// The reference discharge curve is stretched linearly so that `full_mv` maps
// to 100% and `empty_mv` to 0%; with the reference endpoints it is used as
// measured, and other values adapt it to a different battery or divider.
// Voltage is only a rough proxy for charge: load, temperature, age, and
// charging all shift it.
uint8_t percent_from_mv(uint32_t mv, uint32_t empty_mv = kReferenceEmptyMv,
                        uint32_t full_mv = kReferenceFullMv);

}  // namespace fy::battery
