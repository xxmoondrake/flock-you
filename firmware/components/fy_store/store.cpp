// SPDX-License-Identifier: MIT

#include "fy_store.h"

#include <cstring>

namespace fy::store {
namespace {

constexpr size_t kCrcOffset = offsetof(RecordHeader, crc);

uint32_t header_seq(const void *record) {
  uint32_t seq;
  memcpy(&seq, static_cast<const uint8_t *>(record) + offsetof(RecordHeader, seq), sizeof(seq));
  return seq;
}

uint32_t record_crc(const void *record) {
  const uint8_t *p = static_cast<const uint8_t *>(record);
  const uint32_t crc = crc32(p, kCrcOffset);
  return crc32(p + kCrcOffset + 4, kRecordSize - kCrcOffset - 4, crc);
}

// Copies at most kNameLen - 1 characters; `dst` must already be zeroed.
void copy_name(char *dst, const char *src) {
  for (size_t i = 0; i + 1 < kNameLen && src[i]; i++) dst[i] = src[i];
}

Observation observation_of(const Sighting &s) {
  Observation o = {};
  o.uptime_ms = s.uptime_ms;
  o.utc = s.utc;
  o.rssi = s.rssi;
  o.channel = s.channel;
  o.loc = s.loc;
  return o;
}

}  // namespace

// ---- Records ----------------------------------------------------------------------

uint32_t crc32(const void *data, size_t len, uint32_t crc) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void seal(void *record) {
  uint8_t *p = static_cast<uint8_t *>(record);
  const uint32_t magic = kRecordMagic;
  memcpy(p + offsetof(RecordHeader, magic), &magic, sizeof(magic));
  p[offsetof(RecordHeader, version)] = kRecordVersion;
  const uint32_t crc = record_crc(p);
  memcpy(p + kCrcOffset, &crc, sizeof(crc));
}

bool is_valid(const void *record) {
  const uint8_t *p = static_cast<const uint8_t *>(record);
  uint32_t magic, crc;
  memcpy(&magic, p + offsetof(RecordHeader, magic), sizeof(magic));
  if (magic != kRecordMagic || p[offsetof(RecordHeader, version)] != kRecordVersion) return false;
  memcpy(&crc, p + kCrcOffset, sizeof(crc));
  return crc == record_crc(p);
}

uint8_t record_type(const void *record) {
  return static_cast<const uint8_t *>(record)[offsetof(RecordHeader, type)];
}

uint32_t unix_seconds(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                      uint8_t second) {
  // Days since 1970-01-01 (civil-from-days inverse, Howard Hinnant's method).
  const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t mp = month > 2 ? month - 3 : month + 9;
  const int64_t doy = (153 * mp + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  return static_cast<uint32_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

// ---- Flash log --------------------------------------------------------------------

bool RecordLog::slot_blank(size_t slot) {
  uint8_t buf[kRecordSize];
  if (!flash_.read(slot * kRecordSize, buf, sizeof(buf))) return false;
  for (uint8_t b : buf) {
    if (b != 0xff) return false;
  }
  return true;
}

bool RecordLog::mount() {
  sectors_ = flash_.size() / kSectorSize;
  if (sectors_ > kMaxSectors) sectors_ = kMaxSectors;
  if (sectors_ < 2) return false;

  // The sector whose first record has the highest sequence is the head.
  uint32_t head_seq = 0;
  size_t head = 0;
  for (size_t s = 0; s < sectors_; s++) {
    uint8_t rec[kRecordSize];
    first_seq_[s] = 0;
    if (flash_.read(s * kSectorSize, rec, sizeof(rec)) && is_valid(rec)) {
      first_seq_[s] = header_seq(rec);
      if (first_seq_[s] > head_seq) {
        head_seq = first_seq_[s];
        head = s;
      }
    }
  }
  if (head_seq == 0) {  // empty log
    write_slot_ = 0;
    next_seq_ = 1;
    return true;
  }

  // Within the head sector: the newest valid record, then the first blank
  // slot after it. Non-blank slots (torn writes) are skipped, not reused.
  uint32_t max_seq = head_seq;
  size_t last_valid = 0;
  for (size_t slot = 0; slot < kSlotsPerSector; slot++) {
    uint8_t rec[kRecordSize];
    if (!flash_.read((head * kSlotsPerSector + slot) * kRecordSize, rec, sizeof(rec))) continue;
    if (is_valid(rec)) {
      last_valid = slot;
      if (header_seq(rec) > max_seq) max_seq = header_seq(rec);
    }
  }
  next_seq_ = max_seq + 1;
  size_t slot = last_valid + 1;
  while (slot < kSlotsPerSector && !slot_blank(head * kSlotsPerSector + slot)) slot++;
  write_slot_ = slot < kSlotsPerSector ? head * kSlotsPerSector + slot
                                       : ((head + 1) % sectors_) * kSlotsPerSector;
  return true;
}

bool RecordLog::append(void *record) {
  if (sectors_ == 0) return false;
  const size_t sector = write_slot_ / kSlotsPerSector;
  const bool sector_start = (write_slot_ % kSlotsPerSector) == 0;
  if (sector_start) {
    // Starting a sector: erase it first (dropping its oldest records).
    first_seq_[sector] = 0;
    if (!flash_.erase_sector(sector * kSectorSize)) return false;
  }
  const uint32_t seq = next_seq_++;
  memcpy(static_cast<uint8_t *>(record) + offsetof(RecordHeader, seq), &seq, sizeof(seq));
  seal(record);
  const bool ok = flash_.write(write_slot_ * kRecordSize, record, kRecordSize);
  if (ok && sector_start) first_seq_[sector] = seq;
  // Advance even after a failed write: the slot may be partly programmed.
  write_slot_ = (write_slot_ + 1) % capacity();
  return ok;
}

bool RecordLog::clear(Progress progress, void *ctx) {
  bool ok = true;
  for (size_t s = 0; s < sectors_; s++) {
    ok = flash_.erase_sector(s * kSectorSize) && ok;
    first_seq_[s] = 0;
    if (progress && ((s + 1) % 16 == 0 || s + 1 == sectors_)) progress(s + 1, sectors_, ctx);
  }
  write_slot_ = 0;  // next_seq_ keeps increasing so sequence numbers stay unique
  return ok;
}

size_t RecordLog::read_sector(size_t sector, uint8_t *out) {
  if (sector >= sectors_ || !flash_.read(sector * kSectorSize, out, kSectorSize)) return 0;
  size_t n = 0;
  for (size_t slot = 0; slot < kSlotsPerSector; slot++) {
    const uint8_t *rec = out + slot * kRecordSize;
    if (!is_valid(rec)) continue;
    if (n != slot) memmove(out + n * kRecordSize, rec, kRecordSize);
    n++;
  }
  return n;
}

size_t RecordLog::used() const {
  uint32_t oldest = 0;
  for (size_t s = 0; s < sectors_; s++) {
    if (first_seq_[s] && (oldest == 0 || first_seq_[s] < oldest)) oldest = first_seq_[s];
  }
  if (oldest == 0) return 0;
  const size_t n = next_seq_ - oldest;
  return n > capacity() ? capacity() : n;
}

// ---- Reading encounters back -------------------------------------------------------

bool EncounterDedup::keep(const EncounterRecord &rec) {
  const uint64_t key = (static_cast<uint64_t>(rec.h.boot) << 32) | rec.encounter;
  // EncounterTracker::start writes the only non-final snapshot with one sighting.
  const bool first_snapshot = rec.count == 1 && !(rec.h.flags & kFlagFinal);
  for (size_t i = 0; i < used_; i++) {
    if (keys_[i] != key) continue;
    if (first_snapshot) keys_[i] = keys_[--used_];  // nothing older remains
    return false;
  }
  if (!first_snapshot && used_ < kCapacity) keys_[used_++] = key;
  return true;
}

// ---- Encounter tracking --------------------------------------------------------

size_t EncounterTracker::active() const {
  size_t n = 0;
  for (const Active &a : active_) n += a.used ? 1 : 0;
  return n;
}

void EncounterTracker::emit(Active &a, uint32_t now_ms) {
  EncounterRecord copy = a.rec;  // the sink seals and writes its own copy
  sink_(copy, ctx_);
  a.dirty = false;
  a.last_write_ms = now_ms;
}

void EncounterTracker::close(Active &a, uint32_t now_ms) {
  a.rec.h.flags |= kFlagFinal;
  emit(a, now_ms);
  a.used = false;
}

void EncounterTracker::start(Active &a, const Sighting &s) {
  a = Active{};
  a.used = true;
  a.rank = s.rank;
  EncounterRecord &r = a.rec;
  r.h.type = kTypeEncounter;
  r.h.boot = boot_;
  r.h.flags = static_cast<uint8_t>((s.specific ? kFlagSpecific : 0) | (s.ble ? kFlagBle : 0));
  r.encounter = next_encounter_++;
  memcpy(r.mac, s.mac, sizeof(r.mac));
  r.count = 1;
  r.rssi_min = r.rssi_max = s.rssi;
  r.rule = s.rule;
  r.first = r.last = r.best = observation_of(s);
  copy_name(r.name, s.name);
  emit(a, s.uptime_ms);  // persist the start immediately
}

void EncounterTracker::on_sighting(const Sighting &s) {
  Active *found = nullptr;
  for (Active &a : active_) {
    if (a.used && memcmp(a.rec.mac, s.mac, sizeof(s.mac)) == 0) {
      found = &a;
      break;
    }
  }
  if (found && static_cast<int32_t>(s.uptime_ms - found->rec.last.uptime_ms) >
                   static_cast<int32_t>(cfg_.absence_ms)) {
    close(*found, s.uptime_ms);  // a new encounter with the same device
    found = nullptr;
  }

  if (found) {
    EncounterRecord &r = found->rec;
    if (r.count < 0xffff) r.count++;
    if (s.rssi < r.rssi_min) r.rssi_min = s.rssi;
    if (s.rssi > r.rssi_max) r.rssi_max = s.rssi;
    r.last = observation_of(s);
    if (s.rssi > r.best.rssi) r.best = r.last;
    if (s.rank > found->rank) {
      found->rank = s.rank;
      r.rule = s.rule;
      if (s.specific) r.h.flags |= kFlagSpecific;
    }
    if (!r.name[0] && s.name[0]) copy_name(r.name, s.name);
    found->dirty = true;
    return;
  }

  Active *slot = nullptr;
  for (Active &a : active_) {
    if (!a.used) {
      slot = &a;
      break;
    }
  }
  if (!slot) {
    // Table full: close the encounter seen least recently.
    slot = &active_[0];
    for (Active &a : active_) {
      if (a.rec.last.uptime_ms < slot->rec.last.uptime_ms) slot = &a;
    }
    close(*slot, s.uptime_ms);
  }
  start(*slot, s);
}

void EncounterTracker::flush(uint32_t now_ms) {
  for (Active &a : active_) {
    if (a.used && a.dirty) emit(a, now_ms);
  }
}

void EncounterTracker::tick(uint32_t now_ms) {
  for (Active &a : active_) {
    if (!a.used) continue;
    if (static_cast<int32_t>(now_ms - a.rec.last.uptime_ms) >
        static_cast<int32_t>(cfg_.absence_ms)) {
      close(a, now_ms);
    } else if (a.dirty && now_ms - a.last_write_ms >= cfg_.checkpoint_ms) {
      emit(a, now_ms);
    }
  }
}

}  // namespace fy::store
