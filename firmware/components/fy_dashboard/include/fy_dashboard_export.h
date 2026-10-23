// SPDX-License-Identifier: MIT
//
// Encounter records as JSON and CSV, for the dashboard's detection list and
// its exports. Pure C++ with no ESP-IDF dependencies so it can be tested on a
// host computer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fy_dashboard_notes.h"
#include "fy_store.h"

namespace fy::dashboard {

// A snapshot of the record log for walk_encounters().
struct LogWalk {
  uint32_t sectors;
  uint32_t newest_sector;  // read first, then each one before it (wrapping)
  uint32_t next_seq;       // records from this seq on postdate the snapshot
  uint32_t since;          // stop at records older than this
  uint32_t revision;       // the log's revision (RecordLog::revision) at the snapshot
};

enum class WalkEnd : uint8_t { Complete, Changed, Stopped };

// Walks a log snapshot newest record first and calls emit(rec) with the
// current snapshot of each encounter updated in records [since, next_seq);
// emit returns false to stop (Stopped). read(sector, buf, &revision) copies
// one sector's valid records to `buf` (kSectorSize bytes) in slot order and
// reports the log's revision as it read them. The walk ends Changed as soon
// as that differs from the snapshot's: records may have left the log during
// the walk, so what was emitted can mix two histories and is not a
// consistent export. Appends that evict nothing leave the snapshot valid:
// their records are past next_seq and skipped.
template <typename Read, typename Emit>
WalkEnd walk_encounters(const LogWalk &walk, uint8_t *buf, store::EncounterDedup &dedup, Read &&read,
                        Emit &&emit) {
  for (uint32_t i = 0; i < walk.sectors; i++) {
    const uint32_t sector = (walk.newest_sector + walk.sectors - i) % walk.sectors;
    uint32_t revision = 0;
    const size_t n = read(sector, buf, &revision);
    if (revision != walk.revision) return WalkEnd::Changed;
    for (size_t j = n; j-- > 0;) {
      const uint8_t *raw = buf + j * store::kRecordSize;
      store::RecordHeader h;
      memcpy(&h, raw, sizeof(h));
      if (h.seq >= walk.next_seq) continue;  // written after the snapshot
      if (h.seq < walk.since) return WalkEnd::Complete;  // all older from here on
      if (h.type != store::kTypeEncounter) continue;
      store::EncounterRecord rec;
      memcpy(&rec, raw, sizeof(rec));
      if (!dedup.keep(rec)) continue;
      if (!emit(rec)) return WalkEnd::Stopped;
    }
  }
  return WalkEnd::Complete;
}

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
