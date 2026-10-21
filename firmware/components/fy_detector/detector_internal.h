// SPDX-License-Identifier: MIT
//
// Types and hooks shared between the capture sources and the matcher.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace fy::detector {

// Signals weaker than this are ignored (upstream floor).
constexpr int8_t kRssiMin = -95;

// Frames are copied up to this length; probe requests with all their IEs fit
// comfortably. Longer frames are truncated and counted.
constexpr size_t kWifiFrameCopyMax = 384;
constexpr size_t kWifiQueueDepth = 24;

// Legacy advertising payload limit.
constexpr size_t kBleAdvMax = 31;
constexpr size_t kBleQueueDepth = 64;

struct WifiCapture {
  int8_t rssi;
  uint8_t channel;
  bool is_mgmt;
  uint16_t len;
  uint8_t frame[kWifiFrameCopyMax];
};

struct BleCapture {
  int8_t rssi;
  uint8_t len;
  uint8_t addr[6];  // display order
  uint8_t data[kBleAdvMax];
};

// Pipeline counters, readable from any task. Drops mean a queue was full and
// a capture was discarded; they are reported, never hidden.
struct Counters {
  std::atomic<uint32_t> wifi_frames{0};  // every management/data frame received
  std::atomic<uint32_t> wifi_hops{0};    // channel changes
  std::atomic<uint32_t> wifi_candidates{0};
  std::atomic<uint32_t> wifi_dropped{0};
  std::atomic<uint32_t> wifi_truncated{0};
  std::atomic<uint32_t> ble_adverts{0};
  std::atomic<uint32_t> ble_dropped{0};
  std::atomic<uint32_t> ble_oversize{0};
  std::atomic<uint32_t> matches{0};
  std::atomic<uint32_t> reported{0};
  std::atomic<uint8_t> channel{0};
  std::atomic<bool> wifi_capture{false};
};

extern Counters g_counters;

// Hand-off from capture callbacks to the matcher. Never blocks; returns false
// and counts a drop when the queue is full.
bool submit_wifi(const WifiCapture &capture);
bool submit_ble(const BleCapture &capture);

esp_err_t wifi_capture_start();
void wifi_capture_set_enabled(bool enabled);

esp_err_t ble_capture_start();

}  // namespace fy::detector
