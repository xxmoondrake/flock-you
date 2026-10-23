// SPDX-License-Identifier: MIT

#include "fy_dashboard_notes.h"

#include <cstring>
#include <initializer_list>

#include "writer.h"

namespace fy::dashboard {
namespace {

constexpr uint8_t kVersion = 1;

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool empty(const Note &n) { return n.label == NoteLabel::None && !n.ignored && !n.text[0]; }

}  // namespace

const char *label_name(NoteLabel label) {
  switch (label) {
    case NoteLabel::Confirmed: return "confirmed";
    case NoteLabel::FalsePositive: return "false_positive";
    default: return "";
  }
}

bool parse_label(const char *text, NoteLabel *label) {
  if (!text) return false;
  for (NoteLabel l : {NoteLabel::None, NoteLabel::Confirmed, NoteLabel::FalsePositive}) {
    if (strcmp(text, label_name(l)) == 0) {
      *label = l;
      return true;
    }
  }
  return false;
}

bool parse_mac(const char *text, uint8_t mac[6]) {
  if (!text || strlen(text) != 17) return false;
  for (int i = 0; i < 6; i++) {
    const int hi = hex_digit(text[i * 3]);
    const int lo = hex_digit(text[i * 3 + 1]);
    if (hi < 0 || lo < 0 || (i < 5 && text[i * 3 + 2] != ':')) return false;
    mac[i] = static_cast<uint8_t>(hi * 16 + lo);
  }
  return true;
}

void clean_note_text(const char *in, char *out) {
  size_t n = 0;
  for (const char *p = in; p && *p && n + 1 < kNoteTextCap; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c >= 0x20 && c < 0x7f && !(n == 0 && c == ' ')) out[n++] = static_cast<char>(c);
  }
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = '\0';
}

const Note *Notes::find(const uint8_t mac[6]) const {
  for (size_t i = 0; i < count_; i++) {
    if (memcmp(notes_[i].mac, mac, 6) == 0) return &notes_[i];
  }
  return nullptr;
}

bool Notes::set(const Note &note) {
  for (size_t i = 0; i < count_; i++) {
    if (memcmp(notes_[i].mac, note.mac, 6) != 0) continue;
    if (empty(note)) {
      notes_[i] = notes_[--count_];
    } else {
      notes_[i] = note;
    }
    return true;
  }
  if (empty(note)) return true;
  if (count_ >= kMax) return false;
  notes_[count_++] = note;
  return true;
}

size_t Notes::serialize(uint8_t *out, size_t cap) const {
  const size_t len = 1 + count_ * kEntryBytes;
  if (!out || cap < len) return 0;
  out[0] = kVersion;
  uint8_t *p = out + 1;
  for (size_t i = 0; i < count_; i++) {
    const Note &n = notes_[i];
    memcpy(p, n.mac, 6);
    p[6] = static_cast<uint8_t>(n.label);
    p[7] = n.ignored ? 1 : 0;
    memcpy(p + 8, n.text, kNoteTextCap);
    p += kEntryBytes;
  }
  return len;
}

bool Notes::deserialize(const uint8_t *in, size_t len) {
  count_ = 0;
  if (!in || len < 1 || in[0] != kVersion || (len - 1) % kEntryBytes != 0 ||
      (len - 1) / kEntryBytes > kMax) {
    return false;
  }
  const size_t n = (len - 1) / kEntryBytes;
  for (size_t i = 0; i < n; i++) {
    const uint8_t *p = in + 1 + i * kEntryBytes;
    Note note = {};
    memcpy(note.mac, p, 6);
    if (p[6] > static_cast<uint8_t>(NoteLabel::FalsePositive)) {
      count_ = 0;
      return false;
    }
    note.label = static_cast<NoteLabel>(p[6]);
    note.ignored = p[7] != 0;
    char raw[kNoteTextCap];
    memcpy(raw, p + 8, kNoteTextCap);
    raw[kNoteTextCap - 1] = '\0';
    clean_note_text(raw, note.text);
    if (!set(note)) {
      count_ = 0;
      return false;
    }
  }
  return true;
}

size_t note_json(const Note &n, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  w.add("{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"label\":\"%s\",\"ignored\":%s,\"note\":",
        n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5], label_name(n.label),
        n.ignored ? "true" : "false");
  w.add_json_string(n.text, kNoteTextCap);
  w.add_char('}');
  return w.finish();
}

size_t notes_json(const Notes &notes, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  w.add_char('[');
  char one[kNoteJsonCap];
  for (size_t i = 0; i < notes.count(); i++) {
    if (i) w.add_char(',');
    if (!note_json(notes.at(i), one, sizeof(one))) return 0;
    w.add("%s", one);
  }
  w.add_char(']');
  return w.finish();
}

}  // namespace fy::dashboard
