// SPDX-License-Identifier: MIT
//
// Detection pipeline: passive Wi-Fi and BLE capture, bounded hand-off queues,
// signature matching in its own task, and reporting.

#pragma once

#include <cstdint>

#include "esp_err.h"

// Starts Wi-Fi capture with channel hopping, passive BLE scanning, the
// matcher task, and the periodic status report. Call once, after NVS and the
// default event loop are initialized.
esp_err_t fy_detector_start();

// Stops or resumes Wi-Fi capture and channel hopping. BLE scanning is not
// affected. Intended for temporary Wi-Fi access sessions.
void fy_detector_set_wifi_capture(bool enabled);

struct fy_detector_match_info {
  bool valid;
  const char *rule_id;  // static string from the signature table
  bool specific;
  bool ble;
  int8_t rssi;
  uint8_t mac[6];
  uint32_t at_ms;  // uptime when reported
};

struct fy_detector_status {
  bool wifi_capture;
  uint8_t channel;
  uint32_t wifi_frames;
  uint32_t ble_adverts;
  uint32_t reported;           // matches reported (after rate limiting)
  uint32_t reported_specific;  // of which specific
  fy_detector_match_info last;           // most recent reported match
  fy_detector_match_info last_specific;  // most recent reported specific match
};

// Copies the current status. Safe from any task.
void fy_detector_get_status(fy_detector_status *out);

// One signature match, before rate limiting.
struct fy_detector_sighting {
  uint8_t mac[6];  // display order
  uint8_t rule;    // fy::Rule
  uint8_t rank;    // higher is a stronger match
  bool specific;
  bool ble;
  int8_t rssi;
  uint8_t channel;  // Wi-Fi channel; 0 for BLE
  char name[33];    // BLE name when present
  uint32_t uptime_ms;
};

// Called from the matcher task for every match. It must return quickly and
// never block (hand the sighting to a queue).
using fy_detector_sighting_hook = void (*)(const fy_detector_sighting &sighting);
void fy_detector_set_sighting_hook(fy_detector_sighting_hook hook);
