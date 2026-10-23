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

  // Stored history (from the log at boot, updated as records are written).
  uint32_t encounter_records;
  uint32_t battery_records;
  uint32_t oldest_utc;  // Unix seconds of the oldest timestamped record; 0 = none

  // Distinct devices per fy::DeviceType, this session and across stored
  // history. When *_saturated is set the counts are lower bounds.
  uint16_t session_devices[kFyRecorderMaxTypes];
  uint16_t stored_devices[kFyRecorderMaxTypes];
  bool session_saturated;
  bool stored_saturated;

  bool erasing;  // an erase is in progress
  uint8_t erase_percent;
  uint32_t saves_completed;  // incremented after each requested save
};

// Mounts the log and starts recording. Call after NVS is initialized and
// before the detector starts.
esp_err_t fy_recorder_start();

// Copies the current status. Safe from any task.
void fy_recorder_get_status(fy_recorder_status *out);

// Asks the recorder to write every changed open encounter now. Completion is
// signalled by saves_completed increasing.
void fy_recorder_request_save();

// Saves open encounters and waits up to `timeout_ms` for completion.
bool fy_recorder_save_and_wait(uint32_t timeout_ms);

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
};

// Returns false when no log is mounted.
bool fy_recorder_log_snapshot(fy_recorder_log_cursor *out);

// Copies the valid records of `sector`, in slot order (oldest first), to
// `buf` (kFyRecorderSectorBytes). Returns how many it copied. Safe from any
// task.
size_t fy_recorder_read_sector(uint32_t sector, uint8_t *buf);

// Asks the recorder to erase every stored record, including battery samples;
// progress is reported in the status. Only call after a deliberate,
// authorized user action: a dashboard login, or on the Heltec the physical
// button with an explicit confirmation step.
void fy_recorder_request_erase();
