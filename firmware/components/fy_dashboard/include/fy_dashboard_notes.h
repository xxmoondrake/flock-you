// SPDX-License-Identifier: MIT
//
// The owner's notes on detected addresses: a label (confirmed or false
// positive), a short note, and whether to hide the address from the list,
// map, and exports. Kept in NVS on the device. Pure C++ with no ESP-IDF
// dependencies so it can be tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::dashboard {

enum class NoteLabel : uint8_t { None = 0, Confirmed = 1, FalsePositive = 2 };

constexpr size_t kNoteTextCap = 64;  // 63 printable ASCII characters and a NUL

struct Note {
  uint8_t mac[6];
  NoteLabel label;
  bool ignored;
  char text[kNoteTextCap];
};

// "confirmed", "false_positive", or "" for None.
const char *label_name(NoteLabel label);
// Parses label_name()'s output; false for anything else.
bool parse_label(const char *text, NoteLabel *label);

// "aa:bb:cc:dd:ee:ff" (either case) to bytes.
bool parse_mac(const char *text, uint8_t mac[6]);

// Copies `in` to `out` keeping printable ASCII only, at most kNoteTextCap - 1
// characters, with surrounding spaces trimmed.
void clean_note_text(const char *in, char *out);

class Notes {
 public:
  static constexpr size_t kMax = 64;  // about 4.6 KB of RAM
  // Serialized size: a version byte, then fixed-size entries.
  static constexpr size_t kEntryBytes = 6 + 1 + 1 + kNoteTextCap;
  static constexpr size_t kMaxBytes = 1 + kMax * kEntryBytes;

  const Note *find(const uint8_t mac[6]) const;
  // The note on an address, or an empty one with its address when there is
  // none. Setting it back undoes a set() on that address.
  Note current(const uint8_t mac[6]) const;
  // Adds or replaces the note for its address; a note with no label, not
  // ignored, and no text removes it. False when the table is full.
  bool set(const Note &note);
  size_t count() const { return count_; }
  const Note &at(size_t i) const { return notes_[i]; }
  void clear() { count_ = 0; }

  size_t serialize(uint8_t *out, size_t cap) const;
  // Replaces the contents; false (and empty) when the data is malformed.
  bool deserialize(const uint8_t *in, size_t len);

 private:
  Note notes_[kMax] = {};
  size_t count_ = 0;
};

// One note as a JSON object {"mac","label","ignored","note"}. Returns the
// length written, or 0 when `cap` is too small.
size_t note_json(const Note &note, char *out, size_t cap);

// Enough for note_json() of any note, even one of nothing but quotes.
constexpr size_t kNoteJsonCap = 80 + 2 * kNoteTextCap;

enum class NoteSaveResult : uint8_t { Saved, Full, NotSaved };

// Stores the whole table; true when it was stored.
using StoreNotes = bool (*)(const Notes &notes, void *ctx);

// Applies `note` and stores the table. When the store fails, the change
// must neither stay in effect nor ride along with a later save: the
// address's previous note is put back and that table stored again, in case
// the failed attempt got far enough to keep the change. `*restored` (when
// given) tells whether that second store succeeded; if not, the stored copy
// may differ until the next successful save.
NoteSaveResult save_note(Notes &notes, const Note &note, StoreNotes store, void *ctx,
                         bool *restored = nullptr);

// All notes as a JSON array of note_json() objects, as the dashboard streams
// them. Returns the length written, or 0 when `cap` is too small.
size_t notes_json(const Notes &notes, char *out, size_t cap);

}  // namespace fy::dashboard
