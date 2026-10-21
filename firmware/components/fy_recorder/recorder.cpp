// SPDX-License-Identifier: MIT

#include "fy_recorder.h"

#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <new>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "fy_battery.h"
#include "fy_detector.h"
#include "fy_gps.h"
#include "fy_signatures.h"
#include "fy_store.h"

namespace {

using fy::store::BatteryRecord;
using fy::store::EncounterRecord;
using fy::store::EncounterTracker;
using fy::store::Location;
using fy::store::LocationSource;
using fy::store::LocationState;
using fy::store::RecordLog;
using fy::store::Sighting;

const char *TAG = "recorder";

constexpr const char *kPartitionLabel = "obslog";
constexpr auto kPartitionSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr size_t kQueueDepth = 32;
constexpr uint32_t kStatusIntervalMs = 60000;
constexpr uint32_t kLoopWaitMs = 250;  // bounds the latency of save/erase requests
// GNSS time older than this (receiver silent) is not used for timestamps.
constexpr uint32_t kMaxTimeAgeMs = 10000;

static_assert(static_cast<int>(fy::DeviceType::Count) <= kFyRecorderMaxTypes,
              "raise kFyRecorderMaxTypes");

class PartitionFlash : public fy::store::Flash {
 public:
  explicit PartitionFlash(const esp_partition_t *p) : p_(p) {}
  size_t size() const override { return p_->size; }
  bool read(size_t off, void *dst, size_t len) override {
    return esp_partition_read(p_, off, dst, len) == ESP_OK;
  }
  bool write(size_t off, const void *src, size_t len) override {
    return esp_partition_write(p_, off, src, len) == ESP_OK;
  }
  bool erase_sector(size_t off) override {
    return esp_partition_erase_range(p_, off, fy::store::kSectorSize) == ESP_OK;
  }

 private:
  const esp_partition_t *p_;
};

PartitionFlash *flash = nullptr;
RecordLog *record_log = nullptr;
EncounterTracker *tracker = nullptr;
SemaphoreHandle_t log_mutex = nullptr;  // guards record_log
QueueHandle_t queue = nullptr;

// Distinct-device counters; touched only by the recorder task (and by start
// before the task exists).
fy::store::DistinctCounter<512> session_devices;
fy::store::DistinctCounter<2048> stored_devices;

std::atomic<bool> save_requested{false};
std::atomic<bool> erase_requested{false};

portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
fy_recorder_status status = {};  // guarded by status_lock

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint8_t type_of_rule(uint8_t rule) {
  return static_cast<uint8_t>(fy::device_type(static_cast<fy::Rule>(rule)));
}

// Boot session counter, persisted in NVS so uptimes from different sessions
// are never confused.
uint32_t next_boot_session() {
  nvs_handle_t h;
  uint32_t boot = 0;
  if (nvs_open("fy", NVS_READWRITE, &h) != ESP_OK) return 0;
  nvs_get_u32(h, "boot", &boot);
  boot++;
  nvs_set_u32(h, "boot", boot);
  nvs_commit(h);
  nvs_close(h);
  return boot;
}

void note_utc(uint32_t utc) {
  if (utc && (status.oldest_utc == 0 || utc < status.oldest_utc)) status.oldest_utc = utc;
}

// Copies the counters into the shared status (recorder task only).
void publish_counts() {
  portENTER_CRITICAL(&status_lock);
  for (int t = 0; t < kFyRecorderMaxTypes; t++) {
    status.session_devices[t] = session_devices.count(static_cast<uint8_t>(t));
    status.stored_devices[t] = stored_devices.count(static_cast<uint8_t>(t));
  }
  status.session_saturated = session_devices.saturated();
  status.stored_saturated = stored_devices.saturated();
  portEXIT_CRITICAL(&status_lock);
}

bool append(void *record) {
  xSemaphoreTake(log_mutex, portMAX_DELAY);
  const bool ok = record_log->append(record);
  const size_t used = record_log->used();
  const size_t capacity = record_log->capacity();
  xSemaphoreGive(log_mutex);

  const uint8_t pct = capacity ? static_cast<uint8_t>(used * 100 / capacity) : 0;
  portENTER_CRITICAL(&status_lock);
  if (ok) {
    status.records_written++;
  } else {
    status.write_errors++;
  }
  status.used = used;
  status.used_percent = pct;
  status.storage_warning = pct >= CONFIG_FY_STORAGE_WARN_PERCENT;
  portEXIT_CRITICAL(&status_lock);
  if (!ok) ESP_LOGE(TAG, "record write failed");
  return ok;
}

void encounter_sink(EncounterRecord &rec, void *) {
  if (!append(&rec)) return;
  stored_devices.add(type_of_rule(rec.rule), rec.mac);
  portENTER_CRITICAL(&status_lock);
  status.encounter_records++;
  note_utc(rec.first.utc);
  portEXIT_CRITICAL(&status_lock);
}

// Time and the detector's own location at this moment. A stale fix is kept
// with its stale state and age; it is never recorded as current.
void capture_context(uint32_t *utc, Location *loc) {
  *utc = 0;
  *loc = Location{};
  fy_gps_status g;
  fy_gps_get_status(&g);
  if (!g.available) return;
  if (g.nav.time_valid && g.time_age_ms < kMaxTimeAgeMs) {
    const fy::gps::UtcTime &t = g.nav.utc;
    *utc = fy::store::unix_seconds(t.year, t.month, t.day, t.hour, t.minute, t.second) +
           g.time_age_ms / 1000;
  }
  if (g.position_age_ms == UINT32_MAX) return;
  const LocationState state = g.position_current ? LocationState::Current : LocationState::Stale;
  loc->lat_e7 = static_cast<int32_t>(lround(g.nav.latitude_deg * 1e7));
  loc->lon_e7 = static_cast<int32_t>(lround(g.nav.longitude_deg * 1e7));
  if (g.nav.hdop_valid) {
    const float h = g.nav.hdop * 10.0f;
    loc->hdop_x10 = static_cast<uint8_t>(h > 255.0f ? 255.0f : h);
  }
  const uint32_t age_s = g.position_age_ms / 1000;
  loc->age_s = static_cast<uint8_t>(age_s > 255 ? 255 : age_s);
  loc->source_state = static_cast<uint8_t>(static_cast<uint8_t>(LocationSource::Gnss) |
                                           (static_cast<uint8_t>(state) << 4));
}

// Detector hook: runs in the matcher task, so it only captures and enqueues.
void on_sighting(const fy_detector_sighting &d) {
  Sighting s = {};
  memcpy(s.mac, d.mac, sizeof(s.mac));
  s.rule = d.rule;
  s.rank = d.rank;
  s.specific = d.specific;
  s.ble = d.ble;
  s.rssi = d.rssi;
  s.channel = d.channel;
  for (size_t i = 0; i + 1 < sizeof(s.name) && d.name[i]; i++) s.name[i] = d.name[i];
  s.uptime_ms = d.uptime_ms;
  uint32_t utc;
  Location loc;
  capture_context(&utc, &loc);  // record fields are packed: no direct pointers
  s.utc = utc;
  s.loc = loc;
  if (xQueueSend(queue, &s, 0) != pdTRUE) {
    portENTER_CRITICAL(&status_lock);
    status.dropped++;
    portEXIT_CRITICAL(&status_lock);
  }
}

void record_battery(uint32_t now) {
  fy_battery_status b;
  fy_battery_get_status(&b);
  if (!b.available) return;
  BatteryRecord r = {};
  r.h.type = fy::store::kTypeBattery;
  r.h.boot = status.boot;
  r.h.flags = b.valid ? fy::store::kBatteryFlagPlausible : 0;
  r.uptime_ms = now;
  uint32_t utc;
  Location unused;
  capture_context(&utc, &unused);
  r.utc = utc;
  r.mv = b.mv;
  r.raw_mv = b.raw_mv;
  r.percent = b.percent;
  if (append(&r)) {
    portENTER_CRITICAL(&status_lock);
    status.battery_records++;
    note_utc(utc);
    portEXIT_CRITICAL(&status_lock);
  }
}

void erase_progress(size_t done, size_t total, void *) {
  portENTER_CRITICAL(&status_lock);
  status.erase_percent = static_cast<uint8_t>(done * 100 / total);
  portEXIT_CRITICAL(&status_lock);
}

void erase_all() {
  ESP_LOGW(TAG, "erasing all records (user request)");
  portENTER_CRITICAL(&status_lock);
  status.erasing = true;
  status.erase_percent = 0;
  portEXIT_CRITICAL(&status_lock);

  xSemaphoreTake(log_mutex, portMAX_DELAY);
  const bool ok = record_log->clear(erase_progress, nullptr);
  xSemaphoreGive(log_mutex);
  stored_devices.reset();

  portENTER_CRITICAL(&status_lock);
  status.erasing = false;
  status.used = 0;
  status.used_percent = 0;
  status.storage_warning = false;
  status.encounter_records = 0;
  status.battery_records = 0;
  status.oldest_utc = 0;
  if (!ok) status.write_errors++;
  portEXIT_CRITICAL(&status_lock);
  publish_counts();
  ESP_LOGW(TAG, "erase %s", ok ? "complete" : "FAILED");
}

void log_status() {
  fy_recorder_status s;
  fy_recorder_get_status(&s);
  ESP_LOGI(TAG,
           "status records=%lu/%lu (%u%%) boot=%lu encounters=%lu active=%lu written=%lu "
           "errors=%lu dropped=%lu%s",
           static_cast<unsigned long>(s.used), static_cast<unsigned long>(s.capacity),
           s.used_percent, static_cast<unsigned long>(s.boot),
           static_cast<unsigned long>(s.encounters_started),
           static_cast<unsigned long>(s.encounters_active),
           static_cast<unsigned long>(s.records_written),
           static_cast<unsigned long>(s.write_errors), static_cast<unsigned long>(s.dropped),
           s.storage_warning ? " STORAGE WARNING: offload records" : "");
}

void recorder_task(void *) {
  const uint32_t battery_interval_ms = CONFIG_FY_BATTERY_LOG_INTERVAL_S * 1000u;
  uint32_t last_battery = now_ms() - battery_interval_ms;  // first sample right away
  uint32_t last_status = now_ms();
  for (;;) {
    Sighting s;
    if (xQueueReceive(queue, &s, pdMS_TO_TICKS(kLoopWaitMs)) == pdTRUE) {
      tracker->on_sighting(s);
      if (session_devices.add(type_of_rule(s.rule), s.mac)) publish_counts();
    }
    const uint32_t now = now_ms();
    tracker->tick(now);

    if (save_requested.exchange(false)) {
      tracker->flush(now);
      portENTER_CRITICAL(&status_lock);
      status.saves_completed++;
      portEXIT_CRITICAL(&status_lock);
    }
    if (erase_requested.exchange(false)) erase_all();

    if (now - last_battery >= battery_interval_ms) {
      last_battery = now;
      record_battery(now);
    }
    portENTER_CRITICAL(&status_lock);
    status.encounters_started = tracker->started();
    status.encounters_active = tracker->active();
    portEXIT_CRITICAL(&status_lock);
    publish_counts();
    if (now - last_status >= kStatusIntervalMs) {
      last_status = now;
      log_status();
    }
  }
}

// Tallies the stored history once at boot for the device screens.
void scan_history() {
  uint32_t encounters = 0, battery = 0, oldest = 0;
  record_log->for_each([&](const uint8_t *rec) {
    uint32_t utc = 0;
    if (fy::store::record_type(rec) == fy::store::kTypeEncounter) {
      EncounterRecord e;
      memcpy(&e, rec, sizeof(e));
      stored_devices.add(type_of_rule(e.rule), e.mac);
      encounters++;
      utc = e.first.utc;
    } else if (fy::store::record_type(rec) == fy::store::kTypeBattery) {
      BatteryRecord b;
      memcpy(&b, rec, sizeof(b));
      battery++;
      utc = b.utc;
    }
    if (utc && (oldest == 0 || utc < oldest)) oldest = utc;
  });
  status.encounter_records = encounters;
  status.battery_records = battery;
  status.oldest_utc = oldest;
}

}  // namespace

esp_err_t fy_recorder_start() {
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kPartitionSubtype, kPartitionLabel);
  if (!p) {
    ESP_LOGE(TAG, "partition '%s' not found; recording disabled", kPartitionLabel);
    return ESP_ERR_NOT_FOUND;
  }
  log_mutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(kQueueDepth, sizeof(Sighting));
  flash = new (std::nothrow) PartitionFlash(p);
  record_log = flash ? new (std::nothrow) RecordLog(*flash) : nullptr;
  if (!log_mutex || !queue || !record_log) return ESP_ERR_NO_MEM;
  if (!record_log->mount()) {
    ESP_LOGE(TAG, "log mount failed");
    return ESP_FAIL;
  }

  const uint32_t boot = next_boot_session();
  fy::store::TrackerConfig cfg;
  cfg.absence_ms = CONFIG_FY_ENCOUNTER_ABSENCE_S * 1000u;
  cfg.checkpoint_ms = CONFIG_FY_ENCOUNTER_CHECKPOINT_S * 1000u;
  tracker = new (std::nothrow) EncounterTracker(boot, cfg, encounter_sink, nullptr);
  if (!tracker) return ESP_ERR_NO_MEM;

  // The recorder task does not exist yet, so status is not shared here.
  const uint32_t scan_start = now_ms();
  scan_history();
  const size_t used = record_log->used();
  const size_t capacity = record_log->capacity();
  status.available = true;
  status.boot = boot;
  status.capacity = capacity;
  status.used = used;
  status.used_percent = static_cast<uint8_t>(used * 100 / capacity);
  status.storage_warning = status.used_percent >= CONFIG_FY_STORAGE_WARN_PERCENT;
  publish_counts();

  if (xTaskCreate(recorder_task, "fy_recorder", 6144, nullptr, 2, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  fy_detector_set_sighting_hook(on_sighting);
  ESP_LOGI(TAG,
           "recording to '%s' (%lu KB): %u/%u records (%lu encounter, %lu battery), boot "
           "session %lu, history scan %lu ms",
           kPartitionLabel, static_cast<unsigned long>(p->size / 1024),
           static_cast<unsigned>(used), static_cast<unsigned>(capacity),
           static_cast<unsigned long>(status.encounter_records),
           static_cast<unsigned long>(status.battery_records), static_cast<unsigned long>(boot),
           static_cast<unsigned long>(now_ms() - scan_start));
  return ESP_OK;
}

void fy_recorder_get_status(fy_recorder_status *out) {
  portENTER_CRITICAL(&status_lock);
  *out = status;
  portEXIT_CRITICAL(&status_lock);
}

void fy_recorder_request_save() { save_requested = true; }

bool fy_recorder_save_and_wait(uint32_t timeout_ms) {
  if (!record_log) return false;
  uint32_t before;
  portENTER_CRITICAL(&status_lock);
  before = status.saves_completed;
  portEXIT_CRITICAL(&status_lock);
  fy_recorder_request_save();
  const uint32_t start = now_ms();
  while (now_ms() - start < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(20));
    portENTER_CRITICAL(&status_lock);
    const bool done = status.saves_completed != before;
    portEXIT_CRITICAL(&status_lock);
    if (done) return true;
  }
  return false;
}

void fy_recorder_request_erase() {
  if (record_log) erase_requested = true;
}
