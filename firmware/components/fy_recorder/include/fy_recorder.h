// SPDX-License-Identifier: MIT
//
// Recorder: persists encounter records built from detector sightings, and
// periodic battery samples when CONFIG_FY_BATTERY_LOG is enabled, to the
// "obslog" flash partition, and keeps the statistics shown on the device's
// screens.

#pragma once

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

// Asks the recorder to erase every stored record, including battery samples;
// progress is reported in the status. Only call after a deliberate,
// authorized user action: a dashboard login, or on the Heltec the physical
// button with an explicit confirmation step.
void fy_recorder_request_erase();
