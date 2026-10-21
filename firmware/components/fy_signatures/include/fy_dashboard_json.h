// SPDX-License-Identifier: MIT
//
// Detection lines in the upstream flock-you serial format, so its Flask
// dashboard (api/flockyou.py) can ingest this firmware's matches. Pure C++ with
// no ESP-IDF dependencies so it can be tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

#include "fy_signatures.h"

namespace fy {

// Channel reported for BLE detections, as upstream does; their frequency is 0.
constexpr uint8_t kDashboardBleChannel = 0xff;

// Copies an untrusted advertised name, replacing anything that is not
// printable ASCII, and the quote and backslash, with '?'. The result needs no
// JSON escaping and cannot carry control sequences.
void sanitize_name(const char *in, char *out, size_t cap);

// Formats one detection as a single-line JSON object, without a newline. The
// fields are those of upstream's emitDetectionJSON, plus this firmware's
// `rule` and `strength`, which the dashboard keeps alongside its own. No
// location is included. Returns the length written, or 0 (and an empty
// string) when `cap` is too small.
size_t format_dashboard_detection(const Match &m, int rssi, uint8_t channel, char *out,
                                  size_t cap);

}  // namespace fy
