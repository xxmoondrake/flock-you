// SPDX-License-Identifier: MIT
//
// Passive BLE scanning with the NimBLE host. Passive scanning never sends
// scan requests, so the detector does not transmit while observing.

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "detector_internal.h"

namespace fy::detector {
namespace {

const char *TAG = "ble_cap";

// 30 ms window every 300 ms (~10% duty), as upstream, so Wi-Fi capture keeps
// most of the shared 2.4 GHz airtime. Units are 0.625 ms.
constexpr uint16_t kScanIntervalUnits = 480;
constexpr uint16_t kScanWindowUnits = 48;

uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;

// Scanning runs only while wanted (not in standby or a Wi-Fi dashboard
// session) and once the host has synced with the controller.
std::atomic<bool> scan_wanted{true};
std::atomic<bool> synced{false};

int gap_event(struct ble_gap_event *event, void *arg);

void start_scan() {
  if (!scan_wanted || !synced || ble_gap_disc_active()) return;
  struct ble_gap_disc_params params = {};
  params.itvl = kScanIntervalUnits;
  params.window = kScanWindowUnits;
  params.passive = 1;
  // Report every advert so repeated sightings and RSSI changes stay visible.
  params.filter_duplicates = 0;
  const int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, nullptr);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);
    return;
  }
  g_counters.ble_scan = true;
}

// Runs in the NimBLE host task: copy and hand off, no matching here.
int gap_event(struct ble_gap_event *event, void *) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      g_counters.ble_adverts++;
      const auto &disc = event->disc;
      if (disc.rssi < kRssiMin) return 0;
      BleCapture capture;
      capture.rssi = disc.rssi;
      // NimBLE stores addresses least significant byte first.
      for (int i = 0; i < 6; i++) capture.addr[i] = disc.addr.val[5 - i];
      size_t len = disc.length_data;
      if (len > kBleAdvMax) {
        len = kBleAdvMax;
        g_counters.ble_oversize++;
      }
      capture.len = static_cast<uint8_t>(len);
      if (len > 0) memcpy(capture.data, disc.data, len);
      submit_ble(capture);
      return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
      // Scanning was stopped by the stack; resume if still wanted.
      g_counters.ble_scan = false;
      start_scan();
      return 0;
    default:
      return 0;
  }
}

void on_sync() {
  const int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    ESP_LOGE(TAG, "address type inference failed: rc=%d", rc);
    return;
  }
  synced = true;
  start_scan();
  ESP_LOGI(TAG, "passive scan ready (window 30 ms, interval 300 ms)%s",
           scan_wanted ? "" : "; paused");
}

void on_reset(int reason) {
  synced = false;
  g_counters.ble_scan = false;
  ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
}

void host_task(void *) {
  nimble_port_run();  // returns only when nimble_port_stop() is called
  nimble_port_freertos_deinit();
}

}  // namespace

esp_err_t ble_capture_start() {
  const esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
    return err;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  nimble_port_freertos_init(host_task);
  return ESP_OK;
}

void ble_capture_set_enabled(bool enabled) {
  scan_wanted = enabled;
  if (enabled) {
    start_scan();  // before sync, on_sync starts it
    return;
  }
  if (synced && ble_gap_disc_active()) {
    const int rc = ble_gap_disc_cancel();
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc_cancel failed: rc=%d", rc);
  }
  g_counters.ble_scan = false;
}

}  // namespace fy::detector
