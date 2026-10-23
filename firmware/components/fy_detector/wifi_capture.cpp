// SPDX-License-Identifier: MIT
//
// Passive Wi-Fi capture: promiscuous receive of management and data frames
// with channel hopping. The radio only listens; nothing is transmitted.
//
// Flock cameras probe while stepping through the 2.4 GHz channels in
// ascending order, about 125 ms per channel, and their own traffic sits on
// whatever channel their network uses, so any channel the region allows can
// carry them. The hopper therefore sweeps every channel of the configured
// regulatory domain, highest first so it meets an ascending camera sweep
// sooner, and dwells 250 ms (two camera steps) on each, as upstream does.
// Upstream's 1/6/11-only plan is an option. 5 GHz is out of reach: the
// ESP32-S3 radio is 2.4 GHz only.

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "detector_internal.h"
#include "fy_signatures.h"

namespace fy::detector {
namespace {

const char *TAG = "wifi_cap";

// Reversed order and a 250 ms dwell (2 x the cameras' observed 125 ms probe
// interval) follow upstream observations credited to nsm_barri.
constexpr uint8_t kUpstreamChannels[] = {11, 6, 1};
constexpr uint8_t kMaxChannel = 13;  // 14 is Japan-only 802.11b
constexpr uint32_t kDwellMs = CONFIG_FY_WIFI_DWELL_MS;

// Written once before the hopper starts.
uint8_t channels[kMaxChannel];
size_t channel_count = 0;

// Every channel the regulatory domain allows, highest first, or upstream's
// three. Falls back to 1-11 if the domain cannot be read.
void build_channel_plan() {
  channel_count = 0;
#if CONFIG_FY_WIFI_CHANNELS_UPSTREAM
  for (uint8_t ch : kUpstreamChannels) channels[channel_count++] = ch;
#else
  wifi_country_t country = {};
  uint8_t first = 1, last = 11;
  if (esp_wifi_get_country(&country) == ESP_OK && country.nchan > 0) {
    first = country.schan;
    last = static_cast<uint8_t>(country.schan + country.nchan - 1);
  }
  if (last > kMaxChannel) last = kMaxChannel;
  for (uint8_t ch = last; ch >= first && ch > 0; ch--) channels[channel_count++] = ch;
#endif
}

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
      const uint8_t ch = channels[index];
      if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        g_counters.channel = ch;
        g_counters.wifi_hops++;
      }
      index = (index + 1) % channel_count;
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
  // The domain sets which channels the radio may tune to (and the Wi-Fi
  // dashboard's transmit rules). 802.11d is off: nearby access points must
  // not change it.
  const char *country = CONFIG_FY_WIFI_COUNTRY;
  if (country[0] && strcmp(country, "01") != 0) {
    ESP_RETURN_ON_ERROR(esp_wifi_set_country_code(country, false), TAG, "set_country_code");
  }
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
  build_channel_plan();
  if (channel_count == 0) return ESP_ERR_INVALID_STATE;

  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filter), TAG, "set_filter");
  ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(&promiscuous_cb), TAG, "set_rx_cb");
  ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channels[0], WIFI_SECOND_CHAN_NONE), TAG,
                      "set_channel");
  g_counters.channel = channels[0];

  if (xTaskCreate(hopper_task, "fy_hop", 3072, nullptr, 5, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  wifi_capture_set_enabled(true);
  char plan[48] = "";
  size_t used = 0;
  for (size_t i = 0; i < channel_count && used + 4 < sizeof(plan); i++) {
    used += static_cast<size_t>(
        snprintf(plan + used, sizeof(plan) - used, i ? "/%u" : "%u", channels[i]));
  }
  ESP_LOGI(TAG, "promiscuous capture started (country %s, channels %s, dwell %lu ms)",
           country[0] ? country : "01", plan, static_cast<unsigned long>(kDwellMs));
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
