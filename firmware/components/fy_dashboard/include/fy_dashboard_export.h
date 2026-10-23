// SPDX-License-Identifier: MIT
//
// Encounter records as JSON and CSV, for the dashboard's detection list and
// its exports. Pure C++ with no ESP-IDF dependencies so it can be tested on a
// host computer.

#pragma once

#include <cstddef>
#include <cstdint>

#include "fy_dashboard_notes.h"
#include "fy_store.h"

namespace fy::dashboard {

// ISO 8601 UTC ("2024-02-29T12:00:00Z", 20 characters and a NUL) for Unix
// seconds; an empty string for 0 (time unknown).
void format_utc(uint32_t unix_s, char out[21]);

// One encounter as a JSON object without a trailing newline, with the
// owner's note on its address (null when none). The detector's own position
// appears only for observations with a current or stale fix. Returns the
// length written, or 0 when `cap` is too small.
size_t encounter_json(const store::EncounterRecord &rec, const Note *note, char *out, size_t cap);

// The CSV header line, ending in CRLF.
const char *encounter_csv_header();

// One encounter as a CSV line ending in CRLF, matching the header. Returns
// the length written, or 0 when `cap` is too small.
size_t encounter_csv(const store::EncounterRecord &rec, const Note *note, char *out, size_t cap);

// Room for one encounter in either format.
constexpr size_t kEncounterTextCap = 1536;

}  // namespace fy::dashboard
