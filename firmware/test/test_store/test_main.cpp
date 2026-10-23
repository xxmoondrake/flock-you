// SPDX-License-Identifier: MIT
//
// Host unit tests for fy_store: pio test -e native

#include <unity.h>

#include <cstring>
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
    memcpy(dst, &data_[off], len);
    return true;
  }
  bool write(size_t off, const void *src, size_t len) override {
    if (off + len > data_.size()) return false;
    const uint8_t *s = static_cast<const uint8_t *>(src);
    for (size_t i = 0; i < len; i++) data_[off + i] &= s[i];
    writes++;
    return true;
  }
  bool erase_sector(size_t off) override {
    if (off % kSectorSize || off >= data_.size()) return false;
    memset(&data_[off], 0xff, kSectorSize);
    erases++;
    return true;
  }
  std::vector<uint8_t> data_;
  int writes = 0;
  int erases = 0;
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
void capture(EncounterRecord &rec, void *) { emitted.push_back(rec); }

}  // namespace

void setUp() { emitted.clear(); }
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

int main(int, char **) {
  UNITY_BEGIN();
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
