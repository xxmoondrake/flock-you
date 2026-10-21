// SPDX-License-Identifier: MIT
//
// Device storage: fixed-size records (encounters and battery samples), a
// power-loss-tolerant append-only ring log over flash, and the tracker that
// groups repeated sightings into encounters. Pure C++ with no ESP-IDF
// dependencies so it can be tested on a host computer; the device supplies a
// Flash implementation.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::store {

// ---- Common record header ------------------------------------------------------

constexpr size_t kRecordSize = 128;
constexpr uint32_t kRecordMagic = 0x31455946;  // "FYE1"
constexpr uint8_t kRecordVersion = 1;

constexpr uint8_t kTypeEncounter = 1;
constexpr uint8_t kTypeBattery = 2;

// Every record starts with this header. `seq` orders records in the log; the
// CRC covers every byte of the record except the crc field itself.
struct __attribute__((packed)) RecordHeader {
  uint32_t magic;
  uint32_t seq;   // assigned on append
  uint32_t crc;
  uint8_t version;
  uint8_t type;   // kType*
  uint8_t flags;  // type-specific
  uint8_t reserved;
  uint32_t boot;  // boot session counter; uptime values are relative to it
};
static_assert(sizeof(RecordHeader) == 20, "record layout");

uint32_t crc32(const void *data, size_t len, uint32_t crc = 0);

// Sets magic, version, and crc of a kRecordSize record (seq already set).
void seal(void *record);
// True when magic, version, and crc of a kRecordSize record are consistent.
bool is_valid(const void *record);
uint8_t record_type(const void *record);

// Unix seconds for a UTC calendar time (proleptic Gregorian, years >= 1970).
uint32_t unix_seconds(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                      uint8_t second);

// ---- Encounter records -----------------------------------------------------------

enum class LocationSource : uint8_t { None = 0, Gnss = 1, Phone = 2 };
enum class LocationState : uint8_t { None = 0, Current = 1, Stale = 2 };

// The detector's own position when a sighting was made, never an asserted
// position of the transmitter. Coordinates are meaningful only when the state
// is Current or Stale; a stale fix is stored as stale, never as current.
struct __attribute__((packed)) Location {
  int32_t lat_e7;        // degrees x 1e7, positive north
  int32_t lon_e7;        // degrees x 1e7, positive east
  uint8_t hdop_x10;      // 0 = unknown, saturates at 25.5
  uint8_t age_s;         // age of the fix when captured, saturates at 255 s
  uint8_t source_state;  // low nibble LocationSource, high nibble LocationState

  LocationSource source() const { return static_cast<LocationSource>(source_state & 0x0f); }
  LocationState state() const { return static_cast<LocationState>(source_state >> 4); }
};

// One sighting: time, signal, and location captured together, so exports
// never combine values from different moments.
struct __attribute__((packed)) Observation {
  uint32_t uptime_ms;
  uint32_t utc;  // Unix seconds from validated GNSS time; 0 = unknown
  int8_t rssi;
  uint8_t channel;  // Wi-Fi channel; 0 for BLE
  Location loc;
};

constexpr uint8_t kFlagSpecific = 0x01;  // the encounter's best rule is specific
constexpr uint8_t kFlagBle = 0x02;
constexpr uint8_t kFlagFinal = 0x04;  // the encounter has closed

constexpr size_t kNameLen = 29;  // NUL-padded; at most 28 characters

// A snapshot of one encounter. An encounter is written several times as it
// develops; the record with the highest seq for a (boot, encounter) pair is
// its current state.
struct __attribute__((packed)) EncounterRecord {
  RecordHeader h;  // type kTypeEncounter, flags kFlag*
  uint32_t encounter;  // unique within the boot session
  uint8_t mac[6];      // display order
  uint16_t count;      // sightings, saturating
  int8_t rssi_min;
  int8_t rssi_max;
  uint8_t rule;  // fy::Rule of the strongest match in the encounter
  uint8_t reserved;
  Observation first;
  Observation last;
  Observation best;  // strongest RSSI
  char name[kNameLen];  // BLE name or SSID, NUL-padded
};
static_assert(sizeof(Location) == 11, "record layout");
static_assert(sizeof(Observation) == 21, "record layout");
static_assert(sizeof(EncounterRecord) == kRecordSize, "record layout");

// ---- Battery records -------------------------------------------------------------

constexpr uint8_t kBatteryFlagPlausible = 0x01;  // a battery appeared connected

// One periodic battery sample, for characterising charge and discharge.
struct __attribute__((packed)) BatteryRecord {
  RecordHeader h;  // type kTypeBattery, flags kBatteryFlag*
  uint32_t uptime_ms;
  uint32_t utc;     // Unix seconds from validated GNSS time; 0 = unknown
  uint16_t mv;      // smoothed battery voltage
  uint16_t raw_mv;  // this sample's average before smoothing
  uint8_t percent;  // estimate at the time, with the then-configured endpoints
  uint8_t reserved[95];
};
static_assert(sizeof(BatteryRecord) == kRecordSize, "record layout");

// ---- Flash log --------------------------------------------------------------------

// Minimal NOR-flash interface. Writes may only clear bits; erase sets a whole
// sector to 0xFF.
class Flash {
 public:
  virtual ~Flash() = default;
  virtual size_t size() const = 0;
  virtual bool read(size_t offset, void *dst, size_t len) = 0;
  virtual bool write(size_t offset, const void *src, size_t len) = 0;
  virtual bool erase_sector(size_t offset) = 0;
};

constexpr size_t kSectorSize = 4096;
constexpr size_t kSlotsPerSector = kSectorSize / kRecordSize;

// Append-only ring of kRecordSize records. A sector is erased just before its
// first slot is written, which discards that sector's oldest records when the
// log wraps. A write torn by power loss fails its CRC, is skipped on mount,
// and is never overwritten in place.
class RecordLog {
 public:
  static constexpr size_t kMaxSectors = 1024;  // bounds the RAM index

  explicit RecordLog(Flash &flash) : flash_(flash) {}

  // Scans the flash to find the newest record and the next write position.
  bool mount();
  // Assigns the next sequence number, seals, and writes a kRecordSize record.
  bool append(void *record);
  // Erases every sector. `progress`, if given, is called with the number of
  // sectors erased so far and the total.
  using Progress = void (*)(size_t done, size_t total, void *ctx);
  bool clear(Progress progress = nullptr, void *ctx = nullptr);

  size_t capacity() const { return sectors_ * kSlotsPerSector; }
  // Records in the log, counting skipped (torn) slots.
  size_t used() const;
  uint32_t next_seq() const { return next_seq_; }

  // Calls fn(const uint8_t *record) for every valid record, oldest first.
  template <typename Fn>
  void for_each(Fn &&fn) {
    if (sectors_ == 0) return;
    const size_t head = write_slot_ / kSlotsPerSector;
    for (size_t i = 1; i <= sectors_; i++) {
      const size_t sector = (head + i) % sectors_;
      for (size_t slot = 0; slot < kSlotsPerSector; slot++) {
        uint8_t rec[kRecordSize];
        if (!flash_.read(sector * kSectorSize + slot * kRecordSize, rec, sizeof(rec))) break;
        if (is_valid(rec)) fn(static_cast<const uint8_t *>(rec));
      }
    }
  }

 private:
  bool slot_blank(size_t slot);

  Flash &flash_;
  size_t sectors_ = 0;
  size_t write_slot_ = 0;  // absolute slot index of the next write
  uint32_t next_seq_ = 1;
  uint32_t first_seq_[kMaxSectors] = {};  // seq in slot 0 of each sector; 0 = none
};

// ---- Encounter tracking --------------------------------------------------------

struct Sighting {
  uint8_t mac[6];
  uint8_t rule;
  uint8_t rank;  // higher is a stronger match; the encounter keeps its best rule
  bool specific;
  bool ble;
  int8_t rssi;
  uint8_t channel;
  char name[kNameLen];
  uint32_t uptime_ms;
  uint32_t utc;
  Location loc;
};

struct TrackerConfig {
  uint32_t absence_ms = 60000;     // silence that closes an encounter
  uint32_t checkpoint_ms = 60000;  // rewrite interval for a changing encounter
};

// Groups repeated sightings of the same address into encounters and emits
// records to persist: when an encounter starts, at most every checkpoint
// interval while it changes, and once when it closes.
class EncounterTracker {
 public:
  static constexpr size_t kMaxActive = 48;
  using Sink = void (*)(EncounterRecord &rec, void *ctx);

  EncounterTracker(uint32_t boot, const TrackerConfig &cfg, Sink sink, void *ctx)
      : boot_(boot), cfg_(cfg), sink_(sink), ctx_(ctx) {}

  void on_sighting(const Sighting &s);
  // Closes idle encounters and checkpoints changed ones.
  void tick(uint32_t now_ms);
  // Writes every changed open encounter now (for example before power-off).
  void flush(uint32_t now_ms);

  size_t active() const;
  uint32_t started() const { return next_encounter_ - 1; }

 private:
  struct Active {
    bool used = false;
    bool dirty = false;
    uint8_t rank = 0;
    uint32_t last_write_ms = 0;
    EncounterRecord rec = {};
  };

  void start(Active &a, const Sighting &s);
  void emit(Active &a, uint32_t now_ms);
  void close(Active &a, uint32_t now_ms);

  uint32_t boot_;
  TrackerConfig cfg_;
  Sink sink_;
  void *ctx_;
  uint32_t next_encounter_ = 1;
  Active active_[kMaxActive];
};

// ---- Distinct device counting ----------------------------------------------------

// Counts distinct (type, MAC) pairs per type in bounded memory. When the
// table fills, further new devices are not counted and saturated() reports
// that the counts are lower bounds.
template <size_t Capacity>
class DistinctCounter {
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

 public:
  static constexpr size_t kMaxTypes = 32;

  // Returns true when the pair was not seen before.
  bool add(uint8_t type, const uint8_t mac[6]) {
    if (type >= kMaxTypes) return false;
    uint64_t key = static_cast<uint64_t>(type + 1) << 48;
    for (int i = 0; i < 6; i++) key |= static_cast<uint64_t>(mac[i]) << (8 * (5 - i));
    uint64_t h = key * 0x9e3779b97f4a7c15ull;
    for (size_t probe = 0; probe < Capacity; probe++) {
      uint64_t &slot = keys_[(h >> 32) & (Capacity - 1)];
      if (slot == key) return false;
      if (slot == 0) {
        if (used_ >= Capacity * 7 / 8) {  // keep probing short; stop counting
          saturated_ = true;
          return false;
        }
        slot = key;
        used_++;
        counts_[type]++;
        return true;
      }
      h += 1ull << 32;
    }
    saturated_ = true;
    return false;
  }

  uint16_t count(uint8_t type) const { return type < kMaxTypes ? counts_[type] : 0; }
  bool saturated() const { return saturated_; }
  void reset() {  // in place: the table is too large for a stack temporary
    for (uint64_t &k : keys_) k = 0;
    for (uint16_t &c : counts_) c = 0;
    used_ = 0;
    saturated_ = false;
  }

 private:
  uint64_t keys_[Capacity] = {};
  uint16_t counts_[kMaxTypes] = {};
  size_t used_ = 0;
  bool saturated_ = false;
};

}  // namespace fy::store
