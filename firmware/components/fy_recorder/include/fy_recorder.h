// SPDX-License-Identifier: MIT
//
// Recorder: persists encounter records built from detector sightings, and
// periodic battery samples when CONFIG_FY_BATTERY_LOG is enabled, to the
// "obslog" flash partition, and keeps the statistics shown on the device's
// screens.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

constexpr int kFyRecorderMaxTypes = 24;  // device types tracked for counts

struct fy_recorder_status {
  bool available;        // the log partition is mounted
  uint32_t boot;         // this boot session's counter
  uint32_t capacity;     // record slots
  uint32_t used;         // records currently stored
  uint8_t used_percent;
  bool storage_warning;  // used_percent at or above the offload threshold
  uint32_t encounters_started;  // this boot
  uint32_t encounters_active;
  uint32_t records_written;  // this boot
  uint32_t write_errors;
  uint32_t dropped;  // sightings lost because the queue was full
  uint32_t erase_dropped;  // sightings captured before or during an erase, not recorded
  uint32_t encounters_dropped;  // encounters given up unsaved while writes kept failing

  // Stored history: the records the log holds now. The record counts are
  // exact. Old records leaving the log start a rebuild of oldest_utc and
  // stored_devices; while stored_pending is set they still describe the last
  // complete tally.
  uint32_t encounter_records;
  uint32_t battery_records;
  uint32_t oldest_utc;  // Unix seconds of the oldest timestamped record; 0 = none
  bool stored_pending;

  // Distinct devices per fy::DeviceType, this session and across stored
  // history. When *_saturated is set the counts are lower bounds.
  uint16_t session_devices[kFyRecorderMaxTypes];
  uint16_t stored_devices[kFyRecorderMaxTypes];
  bool session_saturated;
  bool stored_saturated;

  bool erasing;  // an erase is in progress
  uint8_t erase_percent;
  bool last_erase_ok;  // the latest erase removed every record
  uint32_t clears;     // erases this boot
  uint32_t save_answered;  // the latest ticket a finished save covered (fy_recorder_request_save)
  bool last_save_ok;  // that save persisted everything accepted since the save before it
};

// Mounts the log and starts recording. Call after NVS is initialized and
// before the detector starts.
esp_err_t fy_recorder_start();

// Copies the current status. Safe from any task.
void fy_recorder_get_status(fy_recorder_status *out);

// Asks the recorder to write every changed open encounter now, including
// sightings queued before the request, and returns a ticket. The request is
// answered once save_answered reaches the ticket (fy::store::SaveLedger::
// answers); last_save_ok is then that save's result, which also reports
// encounters given up since the previous save.
uint32_t fy_recorder_request_save();

enum fy_recorder_save_result {
  FY_RECORDER_SAVED,
  FY_RECORDER_SAVE_FAILED,   // completed, but a write failed; the changes stay pending
  FY_RECORDER_SAVE_TIMEOUT,  // not completed in time
};

// Saves open encounters and waits up to `timeout_ms` for the result.
fy_recorder_save_result fy_recorder_save_and_wait(uint32_t timeout_ms);

// ---- The dashboard browser's clock -------------------------------------------------

// Sets the clock for sightings without GNSS time from a browser's Unix time
// in milliseconds. Kept until reboot; ignored when before 2025.
void fy_recorder_set_phone_time(uint64_t unix_ms);

// True once a browser's clock has been set this boot.
bool fy_recorder_phone_time_set();

// ---- Reading stored records back (the dashboard) ----------------------------------

// The log is read one 4 KB sector at a time, so the recorder waits at most
// for one sector read and keeps writing in between.
constexpr size_t kFyRecorderSectorBytes = 4096;

struct fy_recorder_log_cursor {
  uint32_t sectors;
  uint32_t newest_sector;  // read it first, then each one before it (wrapping)
  uint32_t next_seq;     // records with this seq or later postdate the snapshot
  uint32_t oldest_seq;   // records before this one have left the log; 0 = empty
  uint32_t revision;     // changes whenever records may have left the log
  uint32_t boot;         // with clears, identifies the log's history: an erase
  uint32_t clears;       // or a reboot changes it
};

// Returns false when no log is mounted.
bool fy_recorder_log_snapshot(fy_recorder_log_cursor *out);

// Copies the valid records of `sector`, in slot order (oldest first), to
// `buf` (kFyRecorderSectorBytes), and the log's revision as it read them
// (see fy_recorder_log_cursor) to `revision`. Returns how many it copied.
// Safe from any task.
size_t fy_recorder_read_sector(uint32_t sector, uint8_t *buf, uint32_t *revision);

// Asks the recorder to erase every stored record, including battery samples;
// progress is reported in the status. Only call after a deliberate,
// authorized user action: a dashboard login, or on the Heltec the physical
// button with an explicit confirmation step.
void fy_recorder_request_erase();
