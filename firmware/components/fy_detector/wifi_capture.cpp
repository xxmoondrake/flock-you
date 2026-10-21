// SPDX-License-Identifier: MIT
//
// Passive Wi-Fi capture: promiscuous receive of management and data frames
// with channel hopping. The radio only listens; nothing is transmitted.
// Channel order and dwell are carried over from upstream flock-you.

#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "detector_internal.h"
#include "fy_signatures.h"

namespace fy::detector {
namespace {

const char *TAG = "wifi_cap";

// Reversed order and a 250 ms dwell (2 x the cameras' observed 125 ms probe
// interval) follow upstream observations credited to nsm_barri.
constexpr uint8_t kChannels[] = {11, 6, 1};
constexpr size_t kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);
constexpr uint32_t kDwellMs = 250;

// The promiscuous callback runs only in the Wi-Fi driver task, so one
// scratch buffer suffices and keeps the copy off that task's stack.
WifiCapture scratch;

// Runs in the Wi-Fi driver task: keep it short. Only the OUI pre-filter
// runs here; full matching happens in the matcher task.
void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
  g_counters.wifi_frames++;
  const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
  const int8_t rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
  if (rssi < kRssiMin) return;

  const bool is_mgmt = (type == WIFI_PKT_MGMT);
  const size_t len = pkt->rx_ctrl.sig_len;
  if (!fy::wifi_frame_is_candidate(pkt->payload, len, is_mgmt)) return;

  g_counters.wifi_candidates++;
  size_t copy = len;
  if (copy > kWifiFrameCopyMax) {
    copy = kWifiFrameCopyMax;
    g_counters.wifi_truncated++;
  }
  scratch.rssi = rssi;
  scratch.channel = static_cast<uint8_t>(pkt->rx_ctrl.channel);
  scratch.is_mgmt = is_mgmt;
  scratch.len = static_cast<uint16_t>(copy);
  memcpy(scratch.frame, pkt->payload, copy);
  submit_wifi(scratch);
}

void hopper_task(void *) {
  size_t index = 0;
  for (;;) {
    if (g_counters.wifi_capture.load()) {
      const uint8_t ch = kChannels[index];
      if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        g_counters.channel = ch;
        g_counters.wifi_hops++;
      }
      index = (index + 1) % kChannelCount;
    }
    vTaskDelay(pdMS_TO_TICKS(kDwellMs));
  }
}

}  // namespace

esp_err_t wifi_capture_start() {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_NULL), TAG, "set_mode");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filter), TAG, "set_filter");
  ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(&promiscuous_cb), TAG, "set_rx_cb");
  ESP_RETURN_ON_ERROR(esp_wifi_set_channel(kChannels[0], WIFI_SECOND_CHAN_NONE), TAG,
                      "set_channel");
  g_counters.channel = kChannels[0];

  if (xTaskCreate(hopper_task, "fy_hop", 3072, nullptr, 5, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  wifi_capture_set_enabled(true);
  ESP_LOGI(TAG, "promiscuous capture started (channels 11/6/1, dwell %lu ms)",
           static_cast<unsigned long>(kDwellMs));
  return ESP_OK;
}

void wifi_capture_set_enabled(bool enabled) {
  const esp_err_t err = esp_wifi_set_promiscuous(enabled);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_promiscuous(%d) failed: %s", enabled ? 1 : 0, esp_err_to_name(err));
    return;
  }
  g_counters.wifi_capture = enabled;
}

}  // namespace fy::detector
