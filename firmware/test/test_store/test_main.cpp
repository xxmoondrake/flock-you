// SPDX-License-Identifier: MIT
//
// Host unit tests for fy_store: pio test -e native

#include <unity.h>

#include <cstring>
#include <deque>
#include <vector>

#include "fy_store.h"

using namespace fy::store;

namespace {

// RAM flash with NOR semantics: writes can only clear bits.
class MemFlash : public Flash {
 public:
  explicit MemFlash(size_t sectors) : data_(sectors * kSectorSize, 0xff) {}
  size_t size() const override { return data_.size(); }
  bool read(size_t off, void *dst, size_t len) override {
    if (off + len > data_.size()) return false;
    if (reads_left == 0) return false;
    if (reads_left > 0) reads_left--;
    memcpy(dst, &data_[off], len);
    return true;
  }
  // Write number `fail_write` (counting from 0) fails: not programmed at all,
  // or only its first half when `torn` is set.
  bool write(size_t off, const void *src, size_t len) override {
    if (off + len > data_.size()) return false;
    const uint8_t *s = static_cast<const uint8_t *>(src);
    const bool fail = writes == fail_write || failing;
    const size_t n = fail ? (torn ? len / 2 : 0) : len;
    for (size_t i = 0; i < n; i++) data_[off + i] &= s[i];
    writes++;
    return !fail;
  }
  // While `erase_fails` is set, an erase fails after erasing only its first
  // `partial_erase` bytes (0: nothing changes).
  bool erase_sector(size_t off) override {
    if (off % kSectorSize || off >= data_.size()) return false;
    if (erase_fails) {
      memset(&data_[off], 0xff, partial_erase);
      return false;
    }
    memset(&data_[off], 0xff, kSectorSize);
    erases++;
    return true;
  }
  bool erase_fails = false;
  size_t partial_erase = 0;
  std::vector<uint8_t> data_;
  int writes = 0;
  int erases = 0;
  int fail_write = -1;
  bool torn = false;
  bool failing = false;  // every write fails while set
  long reads_left = -1;  // reads fail once this reaches 0; -1 = never
};

BatteryRecord battery(uint32_t uptime_ms, uint16_t mv) {
  BatteryRecord r = {};
  r.h.type = kTypeBattery;
  r.h.boot = 7;
  r.uptime_ms = uptime_ms;
  r.mv = mv;
  return r;
}

std::vector<uint32_t> seqs(RecordLog &log) {
  std::vector<uint32_t> out;
  log.for_each([&](const uint8_t *rec) {
    RecordHeader h;
    memcpy(&h, rec, sizeof(h));
    out.push_back(h.seq);
  });
  return out;
}

Sighting sighting(uint8_t last_mac_byte, uint32_t t, int8_t rssi, uint8_t rank = 1) {
  Sighting s = {};
  const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0, 0, last_mac_byte};
  memcpy(s.mac, mac, 6);
  s.rule = rank;
  s.rank = rank;
  s.rssi = rssi;
  s.uptime_ms = t;
  s.loc.lat_e7 = static_cast<int32_t>(t);  // distinct per sighting, to check tuples
  return s;
}

std::vector<EncounterRecord> emitted;
bool sink_fails = false;
int fail_next_writes = 0;  // the next this many writes fail
bool capture(EncounterRecord &rec, void *) {
  if (fail_next_writes > 0) {
    fail_next_writes--;
    return false;
  }
  if (sink_fails) return false;
  emitted.push_back(rec);
  return true;
}

}  // namespace

void setUp() {
  emitted.clear();
  sink_fails = false;
  fail_next_writes = 0;
}
void tearDown() {}

// ---- Records ----------------------------------------------------------------------

void test_crc32_reference_value() {
  TEST_ASSERT_EQUAL_HEX32(0xcbf43926, crc32("123456789", 9));  // standard check value
}

void test_seal_and_validate() {
  BatteryRecord r = battery(1000, 3850);
  r.h.seq = 5;
  TEST_ASSERT_FALSE(is_valid(&r));
  seal(&r);
  TEST_ASSERT_TRUE(is_valid(&r));
  TEST_ASSERT_EQUAL_UINT8(kTypeBattery, record_type(&r));
  r.mv ^= 1;  // any change breaks the CRC
  TEST_ASSERT_FALSE(is_valid(&r));
}

void test_unix_seconds() {
  TEST_ASSERT_EQUAL_UINT32(0, unix_seconds(1970, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(946684800, unix_seconds(2000, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(1709208000, unix_seconds(2024, 2, 29, 12, 0, 0));
}

// ---- Flash log --------------------------------------------------------------------

void test_empty_log_mounts() {
  MemFlash f(4);
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(4 * kSlotsPerSector, log.capacity());
  TEST_ASSERT_EQUAL_UINT32(0, log.used());
  TEST_ASSERT_EQUAL_UINT32(1, log.next_seq());
}

void test_append_and_remount_continues() {
  MemFlash f(4);
  {
    RecordLog log(f);
    log.mount();
    for (int i = 0; i < 40; i++) {
      BatteryRecord r = battery(i * 60000, 3800 + i);
      TEST_ASSERT_TRUE(log.append(&r));
    }
  }
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(41, log.next_seq());
  TEST_ASSERT_EQUAL_UINT32(40, log.used());
  BatteryRecord r = battery(0, 0);
  log.append(&r);
  const auto s = seqs(log);
  TEST_ASSERT_EQUAL_UINT32(41, s.size());
  for (size_t i = 0; i < s.size(); i++) TEST_ASSERT_EQUAL_UINT32(i + 1, s[i]);
}

void test_wrap_drops_oldest_sector() {
  MemFlash f(3);
  RecordLog log(f);
  log.mount();
  const size_t total = 3 * kSlotsPerSector + 5;  // wraps into the first sector
  for (size_t i = 0; i < total; i++) {
    BatteryRecord r = battery(i, 4000);
    log.append(&r);
  }
  const auto s = seqs(log);
  // The first sector was erased for the last 5 records.
  TEST_ASSERT_EQUAL_UINT32(2 * kSlotsPerSector + 5, s.size());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 1, s.front());
  TEST_ASSERT_EQUAL_UINT32(total, s.back());
  for (size_t i = 1; i < s.size(); i++) TEST_ASSERT_EQUAL_UINT32(s[i - 1] + 1, s[i]);

  RecordLog again(f);
  TEST_ASSERT_TRUE(again.mount());
  TEST_ASSERT_EQUAL_UINT32(total + 1, again.next_seq());
}

void test_torn_write_is_skipped_and_not_reused() {
  MemFlash f(2);
  {
    RecordLog log(f);
    log.mount();
    for (int i = 0; i < 3; i++) {
      BatteryRecord r = battery(i, 3900);
      log.append(&r);
    }
  }
  // Simulate power loss halfway through writing slot 3.
  BatteryRecord torn = battery(99, 3700);
  torn.h.seq = 4;
  seal(&torn);
  for (size_t i = 0; i < kRecordSize / 2; i++) {
    f.data_[3 * kRecordSize + i] &= reinterpret_cast<uint8_t *>(&torn)[i];
  }

  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(3, seqs(log).size());  // torn record ignored
  BatteryRecord r = battery(100, 3800);
  TEST_ASSERT_TRUE(log.append(&r));
  // The new record went to slot 4, after the torn slot, and reads back.
  BatteryRecord back;
  memcpy(&back, &f.data_[4 * kRecordSize], sizeof(back));
  TEST_ASSERT_TRUE(is_valid(&back));
  TEST_ASSERT_EQUAL_UINT16(3800, back.mv);
  TEST_ASSERT_EQUAL_UINT32(4, seqs(log).size());
}

void test_torn_first_slot_of_new_sector() {
  MemFlash f(3);
  {
    RecordLog log(f);
    log.mount();
    for (size_t i = 0; i < kSlotsPerSector; i++) {
      BatteryRecord r = battery(i, 3900);
      log.append(&r);
    }
  }
  // Power lost while writing the first slot of sector 1 (after its erase).
  memset(&f.data_[kSectorSize], 0x00, 16);
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 1, log.next_seq());
  BatteryRecord r = battery(0, 3800);
  TEST_ASSERT_TRUE(log.append(&r));  // re-erases sector 1 and writes slot 0
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 1, seqs(log).size());
}

void test_clear() {
  MemFlash f(2);
  RecordLog log(f);
  log.mount();
  for (int i = 0; i < 10; i++) {
    BatteryRecord r = battery(i, 3900);
    log.append(&r);
  }
  TEST_ASSERT_TRUE(log.clear());
  TEST_ASSERT_EQUAL_UINT32(0, seqs(log).size());
  TEST_ASSERT_EQUAL_UINT32(0, log.used());
  BatteryRecord r = battery(0, 3800);
  TEST_ASSERT_TRUE(log.append(&r));
  TEST_ASSERT_EQUAL_UINT32(1, seqs(log).size());
}

void test_read_sector_skips_invalid_slots() {
  MemFlash f(2);
  RecordLog log(f);
  log.mount();
  for (int i = 0; i < 5; i++) {
    BatteryRecord r = battery(i, 3900 + i);
    log.append(&r);
  }
  f.data_[2 * kRecordSize + 40] ^= 0xff;  // damage the third record
  TEST_ASSERT_EQUAL_UINT32(2, log.sectors());
  TEST_ASSERT_EQUAL_UINT32(0, log.newest_sector());
  static uint8_t buf[kSectorSize];
  TEST_ASSERT_EQUAL_UINT32(4, log.read_sector(0, buf));
  const uint16_t want[] = {3900, 3901, 3903, 3904};
  for (size_t i = 0; i < 4; i++) {
    BatteryRecord r;
    memcpy(&r, buf + i * kRecordSize, sizeof(r));
    TEST_ASSERT_TRUE(is_valid(&r));
    TEST_ASSERT_EQUAL_UINT16(want[i], r.mv);
  }
  TEST_ASSERT_EQUAL_UINT32(0, log.read_sector(1, buf));  // blank
  TEST_ASSERT_EQUAL_UINT32(0, log.read_sector(2, buf));  // out of range
}

void test_newest_sector_when_the_next_write_starts_a_sector() {
  MemFlash f(3);
  RecordLog log(f);
  log.mount();
  TEST_ASSERT_EQUAL_UINT32(2, log.newest_sector());  // empty: any order works
  for (size_t i = 0; i < 2 * kSlotsPerSector; i++) {
    BatteryRecord r = battery(i, 3900);
    log.append(&r);
  }
  // Sector 2 is next and still blank; sector 1 has the newest records.
  TEST_ASSERT_EQUAL_UINT32(1, log.newest_sector());
  for (size_t i = 0; i < kSlotsPerSector; i++) {
    BatteryRecord r = battery(i, 3900);
    log.append(&r);
  }
  // Full: the next write erases sector 0, which holds the oldest records.
  TEST_ASSERT_EQUAL_UINT32(2, log.newest_sector());
  BatteryRecord r = battery(0, 3900);
  log.append(&r);
  TEST_ASSERT_EQUAL_UINT32(0, log.newest_sector());
}

// ---- Encounter tracking --------------------------------------------------------

void test_sightings_group_into_one_encounter() {
  TrackerConfig cfg;
  EncounterTracker t(3, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 1000, -80));
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());  // start is persisted at once
  t.on_sighting(sighting(1, 5000, -60));
  t.on_sighting(sighting(1, 9000, -75));
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());
  TEST_ASSERT_EQUAL_UINT32(1, t.active());

  t.tick(9000 + cfg.absence_ms + 1);  // silence closes it
  TEST_ASSERT_EQUAL_UINT32(2, emitted.size());
  const EncounterRecord &r = emitted.back();
  TEST_ASSERT_TRUE(r.h.flags & kFlagFinal);
  TEST_ASSERT_EQUAL_UINT8(kTypeEncounter, r.h.type);
  TEST_ASSERT_EQUAL_UINT32(3, r.h.boot);
  TEST_ASSERT_EQUAL_UINT16(3, r.count);
  TEST_ASSERT_EQUAL_INT8(-80, r.rssi_min);
  TEST_ASSERT_EQUAL_INT8(-60, r.rssi_max);
  TEST_ASSERT_EQUAL_UINT32(1000, r.first.uptime_ms);
  TEST_ASSERT_EQUAL_UINT32(9000, r.last.uptime_ms);
  // The strongest sighting keeps its own time and location together.
  TEST_ASSERT_EQUAL_UINT32(5000, r.best.uptime_ms);
  TEST_ASSERT_EQUAL_INT32(5000, r.best.loc.lat_e7);
  TEST_ASSERT_EQUAL_UINT32(0, t.active());
}

void test_absence_starts_new_encounter() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 1000, -70));
  t.on_sighting(sighting(1, 1000 + cfg.absence_ms + 500, -70));
  // close of the first, start of the second
  TEST_ASSERT_EQUAL_UINT32(3, emitted.size());
  TEST_ASSERT_TRUE(emitted[1].h.flags & kFlagFinal);
  TEST_ASSERT_EQUAL_UINT32(1, emitted[1].encounter);
  TEST_ASSERT_EQUAL_UINT32(2, emitted[2].encounter);
  TEST_ASSERT_EQUAL_UINT32(2, t.started());
}

void test_checkpoint_while_changing() {
  TrackerConfig cfg;
  cfg.checkpoint_ms = 10000;
  cfg.absence_ms = 60000;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 0, -70));
  t.on_sighting(sighting(1, 4000, -70));
  t.tick(5000);
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());  // checkpoint not due
  t.tick(10000);
  TEST_ASSERT_EQUAL_UINT32(2, emitted.size());  // changed and due
  TEST_ASSERT_FALSE(emitted.back().h.flags & kFlagFinal);
  t.tick(25000);
  TEST_ASSERT_EQUAL_UINT32(2, emitted.size());  // no change since
}

void test_rule_upgrade_and_specific_flag() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  Sighting weak = sighting(1, 0, -70, 1);
  Sighting strong = sighting(1, 1000, -70, 9);
  strong.specific = true;
  strcpy(strong.name, "FS Ext Battery");
  t.on_sighting(weak);
  t.on_sighting(strong);
  t.tick(1000 + cfg.absence_ms + 1);
  const EncounterRecord &r = emitted.back();
  TEST_ASSERT_EQUAL_UINT8(9, r.rule);
  TEST_ASSERT_TRUE(r.h.flags & kFlagSpecific);
  TEST_ASSERT_EQUAL_STRING("FS Ext Battery", r.name);
}

void test_full_table_evicts_least_recent() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  for (size_t i = 0; i < EncounterTracker::kMaxActive; i++) {
    t.on_sighting(sighting(static_cast<uint8_t>(i), 1000 + i, -70));
  }
  emitted.clear();
  t.on_sighting(sighting(200, 5000, -70));
  TEST_ASSERT_EQUAL_UINT32(2, emitted.size());
  TEST_ASSERT_TRUE(emitted[0].h.flags & kFlagFinal);
  TEST_ASSERT_EQUAL_UINT8(0, emitted[0].mac[5]);  // least recently seen
  TEST_ASSERT_EQUAL_UINT8(200, emitted[1].mac[5]);
  TEST_ASSERT_EQUAL_UINT32(EncounterTracker::kMaxActive, t.active());
}

void test_long_name_is_truncated() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  Sighting s = sighting(1, 0, -70);
  memset(s.name, 'x', kNameLen - 1);
  s.name[kNameLen - 1] = '\0';
  t.on_sighting(s);
  TEST_ASSERT_EQUAL_UINT32(kNameLen - 1, strlen(emitted[0].name));
}

void test_tracker_flush_writes_changed_encounters() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 0, -70));
  t.on_sighting(sighting(2, 0, -70));
  t.on_sighting(sighting(1, 1000, -60));  // only device 1 changed since its start
  emitted.clear();
  t.flush(1500);
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());
  TEST_ASSERT_EQUAL_UINT8(1, emitted[0].mac[5]);
  TEST_ASSERT_FALSE(emitted[0].h.flags & kFlagFinal);
  emitted.clear();
  t.flush(2000);
  TEST_ASSERT_EQUAL_UINT32(0, emitted.size());  // nothing changed since
}

// ---- Reading encounters back -------------------------------------------------------

uint64_t encounter_key(const EncounterRecord &r) {
  return (static_cast<uint64_t>(r.h.boot) << 32) | r.encounter;
}

void test_dedup_keeps_only_newest_snapshot() {
  // Interleaved encounters with starts, checkpoints, closes, and a flush,
  // across two boot sessions that reuse encounter numbers.
  TrackerConfig cfg;
  cfg.checkpoint_ms = 10000;
  for (uint32_t boot = 1; boot <= 2; boot++) {
    EncounterTracker t(boot, cfg, capture, nullptr);
    t.on_sighting(sighting(1, 0, -70));
    t.on_sighting(sighting(2, 500, -70));
    t.on_sighting(sighting(1, 4000, -60));
    t.tick(10000);  // checkpoint of device 1
    t.on_sighting(sighting(2, 12000, -50));
    t.on_sighting(sighting(3, 13000, -80));  // never changes: start only
    t.flush(14000);
    t.tick(14000 + cfg.absence_ms + 1);  // closes all three
  }
  std::vector<EncounterRecord> newest;  // what a reader keeps, newest first
  EncounterDedup dedup;
  for (auto it = emitted.rbegin(); it != emitted.rend(); ++it) {
    if (dedup.keep(*it)) newest.push_back(*it);
  }
  TEST_ASSERT_EQUAL_UINT32(6, newest.size());  // three encounters per boot
  for (const EncounterRecord &r : newest) {
    TEST_ASSERT_TRUE(r.h.flags & kFlagFinal);  // the close is the newest snapshot
    for (const EncounterRecord &o : newest) {
      if (&o != &r) TEST_ASSERT_FALSE(encounter_key(o) == encounter_key(r));
    }
  }
}

void test_dedup_keeps_open_encounter_checkpoint() {
  TrackerConfig cfg;
  cfg.checkpoint_ms = 1000;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 0, -70));
  t.on_sighting(sighting(1, 500, -65));
  t.tick(1500);  // checkpoint; then power is lost before the close
  EncounterDedup dedup;
  TEST_ASSERT_TRUE(dedup.keep(emitted[1]));
  TEST_ASSERT_EQUAL_UINT16(2, emitted[1].count);
  TEST_ASSERT_FALSE(dedup.keep(emitted[0]));
}

void test_dedup_overflow_lets_older_snapshots_through() {
  // More encounters open at once than the set holds: the newest snapshot of
  // each still comes first, and only remembered ones drop their older ones.
  constexpr size_t kMany = EncounterDedup::kCapacity + 20;
  std::vector<EncounterRecord> starts, finals;
  for (size_t i = 0; i < kMany; i++) {
    EncounterRecord r = {};
    r.h.boot = 1;
    r.encounter = static_cast<uint32_t>(i + 1);
    r.count = 1;
    starts.push_back(r);
    r.count = 2;
    r.h.flags = kFlagFinal;
    finals.push_back(r);
  }
  EncounterDedup dedup;
  size_t kept_finals = 0, kept_starts = 0;
  for (const EncounterRecord &r : finals) kept_finals += dedup.keep(r) ? 1 : 0;
  for (const EncounterRecord &r : starts) kept_starts += dedup.keep(r) ? 1 : 0;
  TEST_ASSERT_EQUAL_UINT32(kMany, kept_finals);
  TEST_ASSERT_EQUAL_UINT32(kMany - EncounterDedup::kCapacity, kept_starts);
}

void test_clear_reports_progress() {
  MemFlash f(40);
  RecordLog log(f);
  log.mount();
  static size_t last_done, last_total, calls;
  last_done = last_total = calls = 0;
  log.clear(
      [](size_t done, size_t total, void *) {
        last_done = done;
        last_total = total;
        calls++;
      },
      nullptr);
  TEST_ASSERT_EQUAL_UINT32(40, last_done);
  TEST_ASSERT_EQUAL_UINT32(40, last_total);
  TEST_ASSERT_EQUAL_UINT32(3, calls);  // after 16, 32, and the last sector
}

void test_distinct_counter() {
  DistinctCounter<64> c;
  const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t b[6] = {1, 2, 3, 4, 5, 7};
  TEST_ASSERT_TRUE(c.add(0, a));
  TEST_ASSERT_FALSE(c.add(0, a));  // same device again
  TEST_ASSERT_TRUE(c.add(0, b));
  TEST_ASSERT_TRUE(c.add(3, a));   // same MAC, different type
  TEST_ASSERT_EQUAL_UINT16(2, c.count(0));
  TEST_ASSERT_EQUAL_UINT16(1, c.count(3));
  TEST_ASSERT_FALSE(c.saturated());
  c.reset();
  TEST_ASSERT_EQUAL_UINT16(0, c.count(0));
}

void test_distinct_counter_saturates_safely() {
  DistinctCounter<16> c;
  for (uint8_t i = 0; i < 40; i++) {
    const uint8_t mac[6] = {0, 0, 0, 0, 0, i};
    c.add(1, mac);
  }
  TEST_ASSERT_TRUE(c.saturated());
  TEST_ASSERT_EQUAL_UINT16(14, c.count(1));  // stops at 7/8 of capacity
}

// ---- Recovery after failed writes -------------------------------------------

void test_failed_first_write_of_a_sector_keeps_later_records() {
  MemFlash f(3);
  {
    RecordLog log(f);
    log.mount();
    for (size_t i = 0; i < kSlotsPerSector; i++) {
      BatteryRecord r = battery(i, 3900);
      log.append(&r);
    }
    f.fail_write = f.writes;  // the first write of sector 1 fails
    BatteryRecord r = battery(100, 3900);
    TEST_ASSERT_FALSE(log.append(&r));
    for (int i = 0; i < 5; i++) {  // later slots of sector 1 succeed
      BatteryRecord s = battery(101 + i, 3900);
      TEST_ASSERT_TRUE(log.append(&s));
    }
    TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 5, log.used() - 1);  // the failed slot counts as used
  }
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 5, seqs(log).size());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 7, log.next_seq());  // seq 33 failed, 34-38 written
  BatteryRecord r = battery(200, 3800);
  TEST_ASSERT_TRUE(log.append(&r));
  // Appended after the later records, without erasing sector 1 again.
  const std::vector<uint32_t> s = seqs(log);
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 6, s.size());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 7, s.back());
  TEST_ASSERT_EQUAL_UINT32(1, log.oldest_seq());
}

void test_failed_first_write_in_the_initial_sector() {
  MemFlash f(2);
  {
    RecordLog log(f);
    log.mount();
    f.fail_write = 0;
    f.torn = true;  // partly programmed
    BatteryRecord r = battery(0, 3900);
    TEST_ASSERT_FALSE(log.append(&r));
    for (int i = 1; i <= 3; i++) {
      BatteryRecord s = battery(i, 3900);
      TEST_ASSERT_TRUE(log.append(&s));
    }
    TEST_ASSERT_EQUAL_UINT32(2, log.oldest_seq());
  }
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(3, seqs(log).size());
  TEST_ASSERT_EQUAL_UINT32(5, log.next_seq());
  TEST_ASSERT_EQUAL_UINT32(2, log.oldest_seq());
}

void test_failed_first_write_after_wrap() {
  MemFlash f(3);
  {
    RecordLog log(f);
    log.mount();
    for (size_t i = 0; i < 3 * kSlotsPerSector; i++) {
      BatteryRecord r = battery(i, 3900);
      log.append(&r);
    }
    f.fail_write = f.writes;  // wrapping into sector 0: its first write fails
    BatteryRecord r = battery(500, 3900);
    TEST_ASSERT_FALSE(log.append(&r));
    for (int i = 0; i < 4; i++) {
      BatteryRecord s = battery(501 + i, 3900);
      TEST_ASSERT_TRUE(log.append(&s));
    }
  }
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  const std::vector<uint32_t> s = seqs(log);
  TEST_ASSERT_EQUAL_UINT32(2 * kSlotsPerSector + 4, s.size());
  TEST_ASSERT_EQUAL_UINT32(3 * kSlotsPerSector + 5, s.back());  // the newest record survived
  TEST_ASSERT_EQUAL_UINT32(3 * kSlotsPerSector + 6, log.next_seq());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 1, log.oldest_seq());
  BatteryRecord r = battery(600, 3800);
  TEST_ASSERT_TRUE(log.append(&r));
  TEST_ASSERT_EQUAL_UINT32(2 * kSlotsPerSector + 5, seqs(log).size());
}

void test_unreadable_flash_does_not_mount_as_empty() {
  MemFlash f(3);
  {
    RecordLog log(f);
    log.mount();
    for (int i = 0; i < 40; i++) {
      BatteryRecord r = battery(i, 3900);
      log.append(&r);
    }
  }
  // Reads: sectors 0 and 1 (one each), sector 2 (blank: four), then the head
  // sector 1 (four).
  // A tenth read that succeeds leaves the eleventh: the probe of the slot
  // after the head's newest record.
  for (long ok_reads : {0L, 1L, 2L, 5L, 7L, 10L}) {
    f.reads_left = ok_reads;  // fails during the sector scan or the head scan
    RecordLog log(f);
    TEST_ASSERT_FALSE_MESSAGE(log.mount(), "a read failure must fail the mount");
  }
  f.reads_left = -1;
  RecordLog log(f);
  TEST_ASSERT_TRUE(log.mount());
  TEST_ASSERT_EQUAL_UINT32(40, seqs(log).size());
}

// ---- Erase boundary -------------------------------------------------------------

void test_tracker_discard_emits_nothing_and_keeps_numbering() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 0, -70));
  t.on_sighting(sighting(1, 1000, -60));  // dirty
  t.on_sighting(sighting(2, 1000, -60));
  emitted.clear();
  t.discard();
  TEST_ASSERT_EQUAL_UINT32(0, t.active());
  t.flush(2000);
  t.tick(10 * 60000);
  TEST_ASSERT_EQUAL_UINT32(0, emitted.size());  // nothing from before the discard
  t.on_sighting(sighting(1, 3000, -50));
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());
  TEST_ASSERT_EQUAL_UINT32(3, emitted[0].encounter);  // numbers are not reused
  TEST_ASSERT_EQUAL_UINT16(1, emitted[0].count);       // a fresh encounter
}

void test_ingest_gate() {
  IngestGate g;
  const uint32_t before = g.admit();
  TEST_ASSERT_TRUE(before != 0);
  g.close();
  TEST_ASSERT_EQUAL_UINT32(0, g.admit());  // nothing admitted during the boundary
  g.open();
  const uint32_t after = g.admit();
  TEST_ASSERT_TRUE(g.accept(after));
  TEST_ASSERT_FALSE(g.accept(before));  // stamped before the boundary: stale
  TEST_ASSERT_FALSE(g.accept(0));
  TEST_ASSERT_EQUAL_UINT32(3, g.dropped());  // refused, stale, and unstamped
}

// The recorder's erase, step by step: two sightings queued before it, one
// captured before it but queued only after the gate reopened, one refused
// during it, and one new. Each turned-away sighting is counted exactly once.
void test_erase_boundary_counts_each_discarded_sighting_once() {
  IngestGate g;
  std::deque<uint32_t> queue = {g.admit(), g.admit()};
  const uint32_t delayed = g.admit();  // captured; its enqueue is delayed
  g.close();
  TEST_ASSERT_EQUAL_UINT32(0, g.admit());  // refused while erasing
  while (!queue.empty()) {                 // emptied at the boundary
    g.discarded();
    queue.pop_front();
  }
  g.open();
  queue.push_back(delayed);  // lands after the erase
  queue.push_back(g.admit());
  int taken = 0;
  while (!queue.empty()) {
    if (g.accept(queue.front())) taken++;
    queue.pop_front();
  }
  TEST_ASSERT_EQUAL(1, taken);                // only the new one
  TEST_ASSERT_EQUAL_UINT32(4, g.dropped());  // 2 discarded + 1 refused + 1 stale
}

// ---- Save acknowledgement --------------------------------------------------------

void test_failed_writes_stay_pending_and_retry() {
  TrackerConfig cfg;
  cfg.checkpoint_ms = 10000;
  EncounterTracker t(1, cfg, capture, nullptr);
  sink_fails = true;
  t.on_sighting(sighting(1, 0, -70));  // the start write fails
  TEST_ASSERT_FALSE(t.flush(100));
  t.tick(5000);  // before the retry interval: no busy retrying
  sink_fails = false;
  t.tick(6000);
  TEST_ASSERT_EQUAL_UINT32(0, emitted.size());
  t.tick(10100);  // retried once the interval passed
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());
  TEST_ASSERT_TRUE(t.flush(10200));
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());  // nothing left pending
}

void test_flush_reports_failure_and_keeps_changes() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 0, -70));
  t.on_sighting(sighting(1, 1000, -60));
  emitted.clear();
  sink_fails = true;
  TEST_ASSERT_FALSE(t.flush(1500));
  sink_fails = false;
  TEST_ASSERT_TRUE(t.flush(1600));  // the same changes, now persisted
  TEST_ASSERT_EQUAL_UINT32(1, emitted.size());
  TEST_ASSERT_EQUAL_UINT16(2, emitted[0].count);
}

void test_failed_close_keeps_the_encounter_until_written() {
  TrackerConfig cfg;
  cfg.absence_ms = 1000;
  cfg.checkpoint_ms = 10000;
  EncounterTracker t(1, cfg, capture, nullptr);
  t.on_sighting(sighting(1, 0, -70));
  emitted.clear();
  sink_fails = true;
  t.tick(2000);  // closes; the final write fails
  TEST_ASSERT_EQUAL_UINT32(1, t.active());
  // The same device again starts a new encounter; the closing one keeps
  // its own final state.
  sink_fails = false;
  t.on_sighting(sighting(1, 2500, -50));
  TEST_ASSERT_EQUAL_UINT32(2, t.active());
  TEST_ASSERT_EQUAL_UINT32(2, emitted.back().encounter);
  TEST_ASSERT_TRUE(t.flush(3000));  // writes the pending final record
  TEST_ASSERT_TRUE(emitted.back().h.flags & kFlagFinal);
  TEST_ASSERT_EQUAL_UINT32(1, emitted.back().encounter);
  TEST_ASSERT_EQUAL_UINT32(1, t.active());
}

void test_full_table_with_failing_writes_drops_and_counts() {
  TrackerConfig cfg;
  cfg.checkpoint_ms = 1000000;
  EncounterTracker t(1, cfg, capture, nullptr);
  sink_fails = true;
  for (size_t i = 0; i < EncounterTracker::kMaxActive + 10; i++) {
    t.on_sighting(sighting(static_cast<uint8_t>(i), static_cast<uint32_t>(i) * 10, -70));
  }
  TEST_ASSERT_EQUAL_UINT32(EncounterTracker::kMaxActive, t.active());  // bounded
  TEST_ASSERT_EQUAL_UINT32(10, t.dropped());
  TEST_ASSERT_EQUAL_UINT32(0, emitted.size());
}

// ---- Retained-history statistics ------------------------------------------------

uint8_t tally_type(const EncounterRecord &e) { return e.rule % 4; }

EncounterRecord encounter_rec(uint8_t mac_last, uint8_t rule, uint32_t utc, uint32_t boot) {
  EncounterRecord r = {};
  r.h.type = kTypeEncounter;
  r.h.boot = boot;
  r.encounter = mac_last;
  const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0, 0, mac_last};
  memcpy(r.mac, mac, 6);
  r.rule = rule;
  r.first.utc = utc;
  return r;
}

template <size_t N>
void run_to_completion(HistoryTally<N> &t, RecordLog &log) {
  for (int i = 0; i < 10000 && !t.step(log, 1); i++) {
  }
  TEST_ASSERT_FALSE(t.pending());
}

// A fresh tally of the same bytes, as at boot.
template <size_t N>
void assert_matches_fresh_scan(HistoryTally<N> &t, MemFlash &f) {
  RecordLog fresh_log(f);
  TEST_ASSERT_TRUE(fresh_log.mount());
  static HistoryTally<N> fresh(tally_type);
  fresh.reset();
  fresh.rescan();
  run_to_completion(fresh, fresh_log);
  TEST_ASSERT_EQUAL_UINT32(fresh.encounters(), t.encounters());
  TEST_ASSERT_EQUAL_UINT32(fresh.battery(), t.battery());
  TEST_ASSERT_EQUAL_UINT32(fresh.oldest_utc(), t.oldest_utc());
  TEST_ASSERT_EQUAL_UINT32(fresh.newest_boot(), t.newest_boot());
  TEST_ASSERT_EQUAL(fresh.devices().saturated(), t.devices().saturated());
  for (uint8_t type = 0; type < 4; type++) {
    TEST_ASSERT_EQUAL_UINT16(fresh.devices().count(type), t.devices().count(type));
  }
}

struct Wired {
  MemFlash f;
  RecordLog log;
  HistoryTally<64> &tally;
  Wired(size_t sectors, HistoryTally<64> &t) : f(sectors), log(f), tally(t) {
    log.mount();
    tally.reset();
    log.set_eviction_hook([](const RecordLog::Eviction &e, void *ctx) {
      static_cast<HistoryTally<64> *>(ctx)->evicted(e);
    }, &tally);
  }
  template <typename R>
  void write(R r) {
    if (log.append(&r)) tally.added(reinterpret_cast<const uint8_t *>(&r));
  }
};

void test_tally_follows_evictions() {
  static HistoryTally<64> tally(tally_type);
  Wired w(3, tally);
  // Repeated devices, battery samples, and timestamps that rise and fall.
  for (uint32_t i = 0; i < 5 * kSlotsPerSector; i++) {
    if (i % 3 == 0) {
      w.write(battery(i, 3900));
    } else {
      w.write(encounter_rec(static_cast<uint8_t>(i % 11), static_cast<uint8_t>(i % 5),
                            1700000000 + (i * 7919) % 5000, 1 + i / 50));
    }
  }
  TEST_ASSERT_TRUE(tally.pending());  // evictions happened
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
}

void test_tally_counts_records_written_during_a_rescan() {
  static HistoryTally<64> tally(tally_type);
  Wired w(4, tally);
  for (uint32_t i = 0; i < 2 * kSlotsPerSector; i++) {
    w.write(encounter_rec(static_cast<uint8_t>(i % 9), 1, 1700000000 + i, 3));
  }
  tally.rescan();
  TEST_ASSERT_FALSE(tally.step(w.log, 1));
  for (uint32_t i = 0; i < 10; i++) {  // more records while the scan runs
    w.write(encounter_rec(static_cast<uint8_t>(40 + i), 2, 1600000000 + i, 4));
  }
  w.write(battery(1, 3900));
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
  TEST_ASSERT_EQUAL_UINT32(1600000000, tally.oldest_utc());
}

void test_tally_restarts_when_an_eviction_interrupts_it() {
  static HistoryTally<64> tally(tally_type);
  Wired w(2, tally);
  for (uint32_t i = 0; i < 2 * kSlotsPerSector; i++) {
    w.write(encounter_rec(static_cast<uint8_t>(i), 1, 1700000000 + i, 1));
  }
  tally.rescan();
  TEST_ASSERT_FALSE(tally.step(w.log, 1));
  w.write(encounter_rec(200, 3, 1800000000, 2));  // evicts the oldest sector mid-scan
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
}

void test_tally_saturation_matches_fresh_scan() {
  static HistoryTally<64> tally(tally_type);
  Wired w(4, tally);
  for (uint32_t i = 0; i < 3 * kSlotsPerSector; i++) {  // more devices than the table holds
    w.write(encounter_rec(static_cast<uint8_t>(i), 0, 1700000000, 1));
  }
  TEST_ASSERT_TRUE(tally.devices().saturated());
  tally.rescan();
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
}

void test_tally_clear_and_unreadable_log() {
  static HistoryTally<64> tally(tally_type);
  Wired w(2, tally);
  w.write(encounter_rec(1, 1, 1700000000, 1));
  TEST_ASSERT_TRUE(w.log.clear());
  tally.reset();
  TEST_ASSERT_EQUAL_UINT32(0, tally.encounters());
  TEST_ASSERT_FALSE(tally.pending());
  w.write(encounter_rec(2, 1, 1700000001, 1));
  w.f.reads_left = 3;
  tally.rescan();
  TEST_ASSERT_TRUE(tally.step(w.log, 10));  // stops at the unreadable sector
  TEST_ASSERT_TRUE(tally.unavailable());
  TEST_ASSERT_TRUE(tally.pending());  // never presented as a complete tally
  w.f.reads_left = -1;
  tally.rescan();
  run_to_completion(tally, w.log);
  TEST_ASSERT_EQUAL_UINT32(1, tally.encounters());
}

// ---- Save contract -----------------------------------------------------------

// The recorder's save, as recorder_task runs it: sightings queued before the
// save are drained into the tracker, then everything pending is written. An
// encounter given up while draining (its final write failed while the table
// was full) fails the save even though every later write succeeds; the next
// clean save succeeds.
void test_save_reports_an_encounter_lost_while_draining() {
  TrackerConfig cfg;
  cfg.checkpoint_ms = 1000000;
  EncounterTracker t(1, cfg, capture, nullptr);
  SaveLedger saves;
  for (uint8_t i = 0; i < EncounterTracker::kMaxActive; i++) t.on_sighting(sighting(i, i * 10u, -70));
  std::deque<Sighting> queue = {sighting(100, 1000, -60), sighting(101, 1010, -60)};
  const auto drain = [&] {
    while (!queue.empty()) {
      t.on_sighting(queue.front());
      queue.pop_front();
    }
  };
  fail_next_writes = 1;  // the first eviction's final write
  const uint32_t ticket = saves.request();
  TEST_ASSERT_EQUAL_UINT32(ticket, saves.due());
  TEST_ASSERT_FALSE(saves.run(ticket, t, 2000, drain));
  TEST_ASSERT_EQUAL_UINT32(1, t.dropped());
  TEST_ASSERT_TRUE(queue.empty());
  TEST_ASSERT_EQUAL_UINT32(0, saves.due());
  // Nothing lost since: a clean save succeeds.
  const uint32_t next = saves.request();
  TEST_ASSERT_TRUE(saves.run(next, t, 3000, [] {}));
}

// A request made while a save runs is not answered by it.
void test_save_request_during_a_save_gets_the_next_one() {
  TrackerConfig cfg;
  EncounterTracker t(1, cfg, capture, nullptr);
  SaveLedger saves;
  const uint32_t first = saves.request();
  uint32_t second = 0;
  TEST_ASSERT_TRUE(saves.run(saves.due(), t, 100, [&] { second = saves.request(); }));
  TEST_ASSERT_TRUE(SaveLedger::answers(first, first));
  TEST_ASSERT_FALSE(SaveLedger::answers(first, second));  // published answer: `first`
  TEST_ASSERT_EQUAL_UINT32(second, saves.due());           // another save is due
  TEST_ASSERT_TRUE(saves.run(saves.due(), t, 200, [] {}));
  TEST_ASSERT_TRUE(SaveLedger::answers(second, second));
  TEST_ASSERT_EQUAL_UINT32(0, saves.due());
}

// ---- Eviction outcomes ---------------------------------------------------------

// Fills a 3-sector ring exactly, so the next write erases sector 0 (records
// 1-32), with encounters of repeated devices and battery samples.
void fill_ring(Wired &w) {
  for (uint32_t i = 0; i < 3 * kSlotsPerSector; i++) {
    if (i % 4 == 0) {
      w.write(battery(i, 3900));
    } else {
      w.write(encounter_rec(static_cast<uint8_t>(i % 13), static_cast<uint8_t>(i % 5),
                            1700000000 + (i * 7919) % 5000, 1));
    }
  }
}

// The log's own view matches what a fresh mount finds in flash.
void assert_log_matches_flash(RecordLog &log, MemFlash &f) {
  RecordLog fresh(f);
  TEST_ASSERT_TRUE(fresh.mount());
  TEST_ASSERT_EQUAL_UINT32(fresh.oldest_seq(), log.oldest_seq());
  TEST_ASSERT_EQUAL_UINT32(seqs(fresh).size(), seqs(log).size());
}

void test_failed_erase_without_change_keeps_records_counted() {
  static HistoryTally<64> tally(tally_type);
  Wired w(3, tally);
  fill_ring(w);
  run_to_completion(tally, w.log);
  const uint32_t revision = w.log.revision();
  w.f.erase_fails = true;  // fails without touching flash
  w.write(encounter_rec(200, 1, 1800000000, 2));
  TEST_ASSERT_EQUAL_UINT32(revision, w.log.revision());  // nothing left the log
  TEST_ASSERT_EQUAL_UINT32(1, w.log.oldest_seq());       // browsers keep records 1-32
  TEST_ASSERT_FALSE(tally.pending());
  assert_matches_fresh_scan(tally, w.f);
  assert_log_matches_flash(w.log, w.f);
  // Retried once erases work again.
  w.f.erase_fails = false;
  w.write(encounter_rec(201, 1, 1800000001, 2));
  TEST_ASSERT_EQUAL_UINT32(revision + 1, w.log.revision());
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 1, w.log.oldest_seq());
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
  assert_log_matches_flash(w.log, w.f);
}

void test_partly_completed_erase_is_reconciled_from_flash() {
  static HistoryTally<64> tally(tally_type);
  Wired w(3, tally);
  fill_ring(w);
  run_to_completion(tally, w.log);
  const uint32_t revision = w.log.revision();
  w.f.erase_fails = true;
  w.f.partial_erase = kSectorSize / 2;  // records 1-16 are gone, 17-32 remain
  w.write(encounter_rec(200, 1, 1800000000, 2));
  TEST_ASSERT_EQUAL_UINT32(revision + 1, w.log.revision());  // records did leave
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector / 2 + 1, w.log.oldest_seq());
  TEST_ASSERT_FALSE(tally.counts_exact());  // not published as exact meanwhile
  TEST_ASSERT_TRUE(tally.pending());
  run_to_completion(tally, w.log);
  TEST_ASSERT_TRUE(tally.counts_exact());
  assert_matches_fresh_scan(tally, w.f);
  assert_log_matches_flash(w.log, w.f);
  // Retry: the rest of the sector goes, counted exactly.
  w.f.erase_fails = false;
  w.f.partial_erase = 0;
  w.write(encounter_rec(201, 1, 1800000001, 2));
  TEST_ASSERT_EQUAL_UINT32(kSlotsPerSector + 1, w.log.oldest_seq());
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
  assert_log_matches_flash(w.log, w.f);
}

void test_unreadable_sector_before_eviction_is_reconciled() {
  static HistoryTally<64> tally(tally_type);
  Wired w(3, tally);
  fill_ring(w);
  run_to_completion(tally, w.log);
  w.f.reads_left = 0;  // the sector cannot be counted before its erase
  w.write(encounter_rec(200, 1, 1800000000, 2));
  w.f.reads_left = -1;
  TEST_ASSERT_FALSE(tally.counts_exact());
  run_to_completion(tally, w.log);
  assert_matches_fresh_scan(tally, w.f);
  assert_log_matches_flash(w.log, w.f);
}

// ---- Tally recovery through the recorder's scheduling ---------------------------

// Steps the tally the way recorder_task does: only while it wants a step.
// Returns how many passes that took (bounded, to catch busy loops).
int run_scheduled(HistoryTally<64> &t, RecordLog &log) {
  int passes = 0;
  while (t.wants_step() && passes < 1000) {
    t.step(log, 4);
    passes++;
  }
  return passes;
}

void test_tally_recovers_after_a_transient_read_failure() {
  static HistoryTally<64> tally(tally_type);
  Wired w(3, tally);
  for (uint32_t i = 0; i < 2 * kSlotsPerSector; i++) {
    w.write(encounter_rec(static_cast<uint8_t>(i % 7), 1, 1700000000 + i, 1));
  }
  w.f.reads_left = 2;
  tally.rescan();
  TEST_ASSERT_TRUE(run_scheduled(tally, w.log) < 1000);
  TEST_ASSERT_TRUE(tally.unavailable());
  TEST_ASSERT_TRUE(tally.pending());       // never presented as complete
  TEST_ASSERT_FALSE(tally.wants_step());  // and not retried in a busy loop
  // Reads work again; a new request (the recorder's minute retry, or an
  // eviction) resumes the scan through the same predicate.
  w.f.reads_left = -1;
  tally.rescan();
  TEST_ASSERT_TRUE(tally.wants_step());
  TEST_ASSERT_TRUE(run_scheduled(tally, w.log) < 1000);
  TEST_ASSERT_FALSE(tally.pending());
  assert_matches_fresh_scan(tally, w.f);
  // Recovery by eviction alone.
  w.f.reads_left = 2;
  tally.rescan();
  run_scheduled(tally, w.log);
  TEST_ASSERT_TRUE(tally.unavailable());
  w.f.reads_left = -1;
  for (uint32_t i = 0; i < 2 * kSlotsPerSector; i++) {  // wraps: evicts sector 0
    w.write(encounter_rec(static_cast<uint8_t>(50 + i % 5), 2, 1750000000 + i, 2));
  }
  TEST_ASSERT_TRUE(tally.wants_step());
  run_scheduled(tally, w.log);
  TEST_ASSERT_FALSE(tally.pending());
  assert_matches_fresh_scan(tally, w.f);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_erase_boundary_counts_each_discarded_sighting_once);
  RUN_TEST(test_save_reports_an_encounter_lost_while_draining);
  RUN_TEST(test_save_request_during_a_save_gets_the_next_one);
  RUN_TEST(test_failed_erase_without_change_keeps_records_counted);
  RUN_TEST(test_partly_completed_erase_is_reconciled_from_flash);
  RUN_TEST(test_unreadable_sector_before_eviction_is_reconciled);
  RUN_TEST(test_tally_recovers_after_a_transient_read_failure);
  RUN_TEST(test_failed_first_write_of_a_sector_keeps_later_records);
  RUN_TEST(test_failed_first_write_in_the_initial_sector);
  RUN_TEST(test_failed_first_write_after_wrap);
  RUN_TEST(test_unreadable_flash_does_not_mount_as_empty);
  RUN_TEST(test_tracker_discard_emits_nothing_and_keeps_numbering);
  RUN_TEST(test_ingest_gate);
  RUN_TEST(test_failed_writes_stay_pending_and_retry);
  RUN_TEST(test_flush_reports_failure_and_keeps_changes);
  RUN_TEST(test_failed_close_keeps_the_encounter_until_written);
  RUN_TEST(test_full_table_with_failing_writes_drops_and_counts);
  RUN_TEST(test_tally_follows_evictions);
  RUN_TEST(test_tally_counts_records_written_during_a_rescan);
  RUN_TEST(test_tally_restarts_when_an_eviction_interrupts_it);
  RUN_TEST(test_tally_saturation_matches_fresh_scan);
  RUN_TEST(test_tally_clear_and_unreadable_log);
  RUN_TEST(test_tracker_flush_writes_changed_encounters);
  RUN_TEST(test_clear_reports_progress);
  RUN_TEST(test_distinct_counter);
  RUN_TEST(test_distinct_counter_saturates_safely);
  RUN_TEST(test_crc32_reference_value);
  RUN_TEST(test_seal_and_validate);
  RUN_TEST(test_unix_seconds);
  RUN_TEST(test_empty_log_mounts);
  RUN_TEST(test_append_and_remount_continues);
  RUN_TEST(test_wrap_drops_oldest_sector);
  RUN_TEST(test_torn_write_is_skipped_and_not_reused);
  RUN_TEST(test_torn_first_slot_of_new_sector);
  RUN_TEST(test_clear);
  RUN_TEST(test_read_sector_skips_invalid_slots);
  RUN_TEST(test_newest_sector_when_the_next_write_starts_a_sector);
  RUN_TEST(test_dedup_keeps_only_newest_snapshot);
  RUN_TEST(test_dedup_keeps_open_encounter_checkpoint);
  RUN_TEST(test_dedup_overflow_lets_older_snapshots_through);
  RUN_TEST(test_sightings_group_into_one_encounter);
  RUN_TEST(test_absence_starts_new_encounter);
  RUN_TEST(test_checkpoint_while_changing);
  RUN_TEST(test_rule_upgrade_and_specific_flag);
  RUN_TEST(test_full_table_evicts_least_recent);
  RUN_TEST(test_long_name_is_truncated);
  return UNITY_END();
}
