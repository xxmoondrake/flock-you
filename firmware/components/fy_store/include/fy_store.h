// SPDX-License-Identifier: MIT
//
// Device storage: fixed-size records (encounters and battery samples), a
// power-loss-tolerant append-only ring log over flash, and the tracker that
// groups repeated sightings into encounters. Pure C++ with no ESP-IDF
// dependencies so it can be tested on a host computer; the device supplies a
// Flash implementation.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
  uint32_t utc;  // Unix seconds from validated GNSS time, else the dashboard
                 // browser's clock; 0 = unknown
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
  // False when the flash cannot be read: an unreadable log is never taken
  // for an empty one.
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
  // The lowest sequence number still in the log; 0 when it is empty.
  uint32_t oldest_seq() const;
  // Changes whenever records may have left the log (a sector erased to make
  // room, a failed erase that changed flash, or a clear); ordinary appends
  // leave it alone. A reader whose snapshot spans a change may hold a mix.
  uint32_t revision() const { return revision_; }

  // What append() did to the oldest sector to make room. When `known`, the
  // erase succeeded and these counts (read just before it) left the log.
  // Otherwise the outcome is uncertain (the erase failed after changing
  // flash, or the sector could not be read first): reconcile from flash.
  struct Eviction {
    uint32_t encounters;
    uint32_t battery;
    bool known;
  };
  using Evicted = void (*)(const Eviction &eviction, void *ctx);
  void set_eviction_hook(Evicted fn, void *ctx) {
    evicted_ = fn;
    evicted_ctx_ = ctx;
  }
  uint32_t next_seq() const { return next_seq_; }
  size_t sectors() const { return sectors_; }
  // The sector holding the newest records. Reading it and then each sector
  // before it (wrapping) visits records newest sector first. When the next
  // write starts a sector, that sector still holds the oldest records and is
  // visited last.
  size_t newest_sector() const {
    if (sectors_ == 0) return 0;
    const size_t head = write_slot_ / kSlotsPerSector;
    return write_slot_ % kSlotsPerSector ? head : (head + sectors_ - 1) % sectors_;
  }

  // Copies the valid records of `sector`, in slot order (oldest first), to
  // `out`, which holds kSlotsPerSector records. Returns how many it copied.
  size_t read_sector(size_t sector, uint8_t *out);

  // Calls fn(const uint8_t *record) for every valid record of `sector`, in
  // slot order. False when the flash cannot be read. `fn` must not use the
  // log.
  template <typename Fn>
  bool for_each_in_sector(size_t sector, Fn &&fn) {
    for (size_t first = 0; first < kSlotsPerSector; first += kChunkSlots) {
      if (!read_chunk(sector, first)) return false;
      for (size_t i = 0; i < kChunkSlots; i++) {
        const uint8_t *rec = chunk_ + i * kRecordSize;
        if (is_valid(rec)) fn(rec);
      }
    }
    return true;
  }

  // Calls fn(const uint8_t *record) for every valid record, oldest first.
  template <typename Fn>
  void for_each(Fn &&fn) {
    if (sectors_ == 0) return;
    const size_t head = write_slot_ / kSlotsPerSector;
    for (size_t i = 1; i <= sectors_; i++) for_each_in_sector((head + i) % sectors_, fn);
  }

 private:
  // Flash reads cost mostly per call, so scans read several slots at once
  // into a buffer of the log's own (callers serialize use of the log).
  static constexpr size_t kChunkSlots = 8;
  static_assert(kSlotsPerSector % kChunkSlots == 0, "whole chunks per sector");

  enum class Slot : uint8_t { Blank, Programmed, Unreadable };
  Slot probe_slot(size_t slot);
  // Counts the valid records of a sector by type and finds its oldest; false
  // when it cannot be read.
  struct Census {
    uint32_t encounters = 0;
    uint32_t battery = 0;
    uint32_t first_seq = 0;
  };
  bool census(size_t sector, Census *out);
  bool make_room(size_t sector);
  bool read_chunk(size_t sector, size_t first_slot) {
    return flash_.read(sector * kSectorSize + first_slot * kRecordSize, chunk_, sizeof(chunk_));
  }

  Flash &flash_;
  size_t sectors_ = 0;
  size_t write_slot_ = 0;  // absolute slot index of the next write
  uint32_t next_seq_ = 1;
  uint32_t revision_ = 0;
  // The oldest seq in each sector, normally the one in its first slot; 0 =
  // no records.
  uint32_t first_seq_[kMaxSectors] = {};
  Evicted evicted_ = nullptr;
  void *evicted_ctx_ = nullptr;
  uint8_t chunk_[kChunkSlots * kRecordSize] = {};
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
// interval while it changes, and once when it closes. A record the sink
// fails to persist stays pending and is retried at most once per checkpoint
// interval; a closed encounter whose final record is still pending keeps
// its slot until written, unless the table fills, when it is dropped and
// counted.
class EncounterTracker {
 public:
  static constexpr size_t kMaxActive = 48;
  // Returns true when the record was persisted.
  using Sink = bool (*)(EncounterRecord &rec, void *ctx);

  EncounterTracker(uint32_t boot, const TrackerConfig &cfg, Sink sink, void *ctx)
      : boot_(boot), cfg_(cfg), sink_(sink), ctx_(ctx) {}

  void on_sighting(const Sighting &s);
  // Closes idle encounters, checkpoints changed ones, and retries pending
  // writes.
  void tick(uint32_t now_ms);
  // Writes every pending encounter now (for example before power-off).
  // True when all of them were persisted.
  bool flush(uint32_t now_ms);
  // Forgets every open encounter without writing it (the log was cleared).
  // Encounter numbers keep counting, so none is reused.
  void discard();

  size_t active() const;
  uint32_t started() const { return next_encounter_ - 1; }
  // Encounters given up with unpersisted changes because the table was full.
  uint32_t dropped() const { return dropped_; }

 private:
  struct Active {
    bool used = false;
    bool dirty = false;    // changes not yet persisted
    bool closing = false;  // closed; its final record is still pending
    uint8_t rank = 0;
    uint32_t last_write_ms = 0;  // last write attempt
    EncounterRecord rec = {};
  };

  void start(Active &a, const Sighting &s);
  bool emit(Active &a, uint32_t now_ms);
  void close(Active &a, uint32_t now_ms);

  uint32_t boot_;
  TrackerConfig cfg_;
  Sink sink_;
  void *ctx_;
  uint32_t next_encounter_ = 1;
  uint32_t dropped_ = 0;
  Active active_[kMaxActive];
};

// The recorder's saves. Each request gets a ticket; a save covers every
// ticket issued before it starts, so a request made while one runs is
// answered by the next. A save succeeds only when everything accepted since
// the previous save is persisted: its own writes succeed and no encounter
// was given up in between (an older loss is reported once, not forever).
class SaveLedger {
 public:
  // Any task. Returns the ticket to wait for.
  uint32_t request() { return ++requested_; }
  // Recorder task: the ticket a save starting now would cover, or 0.
  uint32_t due() const {
    const uint32_t r = requested_.load();
    return r != covered_ ? r : 0;
  }
  // Recorder task: runs the save covering tickets up to `ticket`.
  // `take_queued` hands the tracker every sighting queued before the save;
  // then every pending encounter is written. Returns the result.
  template <typename TakeQueued>
  bool run(uint32_t ticket, EncounterTracker &tracker, uint32_t now_ms, TakeQueued &&take_queued) {
    take_queued();
    const bool written = tracker.flush(now_ms);
    const bool none_lost = tracker.dropped() == dropped_at_last_save_;
    dropped_at_last_save_ = tracker.dropped();
    covered_ = ticket;
    return written && none_lost;
  }
  // True once a save covering `ticket` has finished (`answered` is the
  // ticket that save covered).
  static bool answers(uint32_t answered, uint32_t ticket) {
    return static_cast<int32_t>(answered - ticket) >= 0;
  }

 private:
  std::atomic<uint32_t> requested_{0};
  uint32_t covered_ = 0;  // recorder task only
  uint32_t dropped_at_last_save_ = 0;
};

// Lets producers on other tasks queue items for the recorder while an erase
// can cut them off. A producer takes a stamp before capturing an item and
// queues it with the item; close() starts a boundary after which no stamp is
// handed out and every earlier stamp is stale, so nothing captured before
// the boundary is accepted after it. Every item turned away is counted once:
// refused by admit(), emptied out of the queue at the boundary (discarded()),
// or found stale when taken (accept()).
class IngestGate {
 public:
  // Producer: the stamp for an item about to be captured, or 0 (counted)
  // while closed. The generation is read before the open flag, so a stamp
  // taken while a close() runs is either refused or already stale.
  uint32_t admit() {
    const uint32_t generation = generation_.load();
    if (open_.load()) return generation;
    dropped_++;
    return 0;
  }
  void close() {
    open_ = false;
    generation_++;
  }
  void open() { open_ = true; }
  // Consumer: an item it emptied out of its queue at the boundary.
  void discarded() { dropped_++; }
  // Consumer: true when an item stamped `stamp` was admitted after the last
  // close(); a stale one is counted.
  bool accept(uint32_t stamp) {
    if (stamp != 0 && stamp == generation_.load()) return true;
    dropped_++;
    return false;
  }
  uint32_t dropped() const { return dropped_.load(); }

 private:
  std::atomic<bool> open_{true};
  std::atomic<uint32_t> generation_{1};
  std::atomic<uint32_t> dropped_{0};
};

// ---- Reading encounters back -------------------------------------------------------

// Keeps only the current snapshot of each encounter while records are read
// newest first. An encounter's first snapshot is its only non-final record
// with a single sighting, so a remembered encounter is forgotten once that
// record is passed; the set therefore holds about as many encounters as were
// open at once (EncounterTracker::kMaxActive). If it still fills, older
// snapshots pass through and readers must keep the first of each.
class EncounterDedup {
 public:
  static constexpr size_t kCapacity = 128;

  // True when `rec` is the newest snapshot of its encounter seen so far.
  bool keep(const EncounterRecord &rec);
  void reset() { used_ = 0; }

 private:
  uint64_t keys_[kCapacity] = {};
  size_t used_ = 0;
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

// ---- Retained history ------------------------------------------------------------

// Statistics over the records the log still holds: encounter and battery
// record counts, the oldest timestamp, distinct devices per type, and the
// newest boot session. The record counts follow writes and evictions
// exactly. Distinct devices and the oldest timestamp cannot be taken back
// when records leave the log, so an eviction (or rescan()) starts a rescan
// that rebuilds them in place, a few sectors per step(); until it completes
// they are not current and pending() is true. Records written during a
// rescan are counted as they arrive and skipped by the scan; an eviction
// during a rescan restarts it.
template <size_t Capacity>
class HistoryTally {
 public:
  // The device type an encounter counts under.
  using TypeOf = uint8_t (*)(const EncounterRecord &rec);

  explicit HistoryTally(TypeOf type_of) : type_of_(type_of) {}

  // The log is empty.
  void reset() {
    encounters_ = battery_ = oldest_utc_ = newest_boot_ = 0;
    devices_.reset();
    scanning_ = restart_ = unavailable_ = false;
    counts_exact_ = true;
  }

  // A record was written to the log.
  void added(const uint8_t *record) {
    RecordHeader h;
    memcpy(&h, record, sizeof(h));
    if (h.boot > newest_boot_) newest_boot_ = h.boot;
    uint32_t utc = 0;
    if (record_type(record) == kTypeEncounter) {
      EncounterRecord e;
      memcpy(&e, record, sizeof(e));
      encounters_++;
      if (scanning_) since_encounters_++;
      devices_.add(type_of_(e), e.mac);
      utc = e.first.utc;
    } else if (record_type(record) == kTypeBattery) {
      BatteryRecord b;
      memcpy(&b, record, sizeof(b));
      battery_++;
      if (scanning_) since_battery_++;
      utc = b.utc;
    }
    note_oldest(&oldest_utc_, utc);
    if (scanning_) note_oldest(&scan_oldest_, utc);
  }

  // Records left the log (RecordLog's eviction hook). An uncertain eviction
  // leaves the counts unknown until a rescan has read the log again.
  void evicted(const RecordLog::Eviction &e) {
    if (e.known) {
      encounters_ -= e.encounters < encounters_ ? e.encounters : encounters_;
      battery_ -= e.battery < battery_ ? e.battery : battery_;
    } else {
      counts_exact_ = false;
    }
    restart_ = true;
  }

  // Rebuilds everything from the log (at mount, after a failed clear, or to
  // retry after a read failure).
  void rescan() { restart_ = true; }

  // True when step() has work: a rescan running or requested. After a read
  // failure this stays false until something requests a rescan, so a
  // persistent failure does not keep the caller busy.
  bool wants_step() const { return scanning_ || restart_; }

  // Scans up to `sectors` sectors of a pending rescan. True when none is
  // pending. A sector that cannot be read leaves the tally unavailable
  // until the next rescan.
  bool step(RecordLog &log, size_t sectors) {
    if (restart_) begin(log);
    for (size_t n = 0; scanning_ && n < sectors; n++) {
      const bool read = log.for_each_in_sector(next_sector_, [&](const uint8_t *rec) {
        RecordHeader h;
        memcpy(&h, rec, sizeof(h));
        if (h.seq >= boundary_) return;  // written during the rescan: already counted
        if (h.boot > newest_boot_) newest_boot_ = h.boot;
        if (record_type(rec) == kTypeEncounter) {
          EncounterRecord e;
          memcpy(&e, rec, sizeof(e));
          scan_encounters_++;
          devices_.add(type_of_(e), e.mac);
          note_oldest(&scan_oldest_, e.first.utc);
        } else if (record_type(rec) == kTypeBattery) {
          BatteryRecord b;
          memcpy(&b, rec, sizeof(b));
          scan_battery_++;
          note_oldest(&scan_oldest_, b.utc);
        }
      });
      if (!read) {
        scanning_ = false;
        unavailable_ = true;
        return true;
      }
      if (++next_sector_ == log.sectors()) {
        scanning_ = false;
        encounters_ = scan_encounters_ + since_encounters_;
        battery_ = scan_battery_ + since_battery_;
        oldest_utc_ = scan_oldest_;
        counts_exact_ = true;
      }
    }
    return !scanning_;
  }

  // Distinct devices and the oldest timestamp are not current.
  bool pending() const { return scanning_ || restart_ || unavailable_; }
  // A rescan failed to read the log.
  bool unavailable() const { return unavailable_; }
  // The record counts match the log (false after an uncertain eviction until
  // a rescan completes).
  bool counts_exact() const { return counts_exact_; }

  uint32_t encounters() const { return encounters_; }
  uint32_t battery() const { return battery_; }
  uint32_t oldest_utc() const { return oldest_utc_; }  // 0 = none; see pending()
  uint32_t newest_boot() const { return newest_boot_; }
  const DistinctCounter<Capacity> &devices() const { return devices_; }  // see pending()

 private:
  static void note_oldest(uint32_t *oldest, uint32_t utc) {
    if (utc && (*oldest == 0 || utc < *oldest)) *oldest = utc;
  }

  void begin(RecordLog &log) {
    restart_ = unavailable_ = false;
    scanning_ = log.sectors() > 0;
    boundary_ = log.next_seq();
    next_sector_ = 0;
    scan_encounters_ = scan_battery_ = scan_oldest_ = 0;
    since_encounters_ = since_battery_ = 0;
    newest_boot_ = 0;  // rebuilt from the scan and from records added meanwhile
    devices_.reset();
    if (!scanning_) encounters_ = battery_ = oldest_utc_ = 0;
  }

  TypeOf type_of_;
  uint32_t encounters_ = 0;
  uint32_t battery_ = 0;
  uint32_t oldest_utc_ = 0;
  uint32_t newest_boot_ = 0;
  DistinctCounter<Capacity> devices_;

  bool scanning_ = false;
  bool restart_ = false;
  bool unavailable_ = false;
  bool counts_exact_ = true;
  uint32_t boundary_ = 0;  // records from this seq on were counted on arrival
  size_t next_sector_ = 0;
  uint32_t scan_encounters_ = 0;
  uint32_t scan_battery_ = 0;
  uint32_t scan_oldest_ = 0;
  uint32_t since_encounters_ = 0;
  uint32_t since_battery_ = 0;
};

}  // namespace fy::store
