// SPDX-License-Identifier: MIT
//
// Matcher and reporter. Capture callbacks hand raw frames and adverts to
// bounded queues; this task matches them against the signature set and
// reports matches, rate-limited per address.

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "detector_internal.h"
#include "fy_board.h"
#include "fy_dashboard_json.h"
#include "fy_detector.h"
#include "fy_gps.h"
#include "fy_signatures.h"
#include "fy_status_led.h"

namespace fy::detector {

Counters g_counters;

namespace {

const char *TAG = "detector";

// Repeat sightings of the same address are reported at most this often,
// unless the match gets stronger (upstream behavior).
constexpr uint32_t kReportCooldownMs = 5000;
constexpr size_t kRecentSlots = 32;
constexpr uint32_t kStatusIntervalMs = 30000;
// Captures handled from each queue per round, so a burst on one transport
// cannot starve the other.
constexpr int kBatch = 8;

QueueHandle_t wifi_queue = nullptr;
QueueHandle_t ble_queue = nullptr;
TaskHandle_t matcher_handle = nullptr;

// Latest reported matches for display; guarded by `status_lock`.
portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
fy_detector_match_info last_match = {};
fy_detector_match_info last_specific = {};
uint32_t reported_specific = 0;

// ---- Rate limiting ----------------------------------------------------------

struct Recent {
  bool used;
  uint8_t mac[6];
  uint8_t rank;  // strength, then upstream tier, of the best match reported
  uint32_t last_report_ms;
};

Recent recent[kRecentSlots];

// Set at startup, before matching produces sightings: the recorder and the
// alerts.
constexpr int kMaxSightingHooks = 2;
fy_detector_sighting_hook sighting_hooks[kMaxSightingHooks] = {};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint8_t rank_of(const Match &m) {
  return static_cast<uint8_t>((static_cast<uint8_t>(m.strength) << 4) |
                              rule_info(m.rule).upstream_tier);
}

// Returns true when this match should be reported. A stronger match for an
// address is reported even inside the cooldown: an upgrade is new
// information, not a repeat.
bool should_report(const Match &m) {
  const uint32_t now = now_ms();
  const uint8_t rank = rank_of(m);
  Recent *oldest = &recent[0];
  for (Recent &r : recent) {
    if (r.used && memcmp(r.mac, m.mac, 6) == 0) {
      const bool cooling = (now - r.last_report_ms) < kReportCooldownMs;
      const bool upgrade = rank > r.rank;
      if (cooling && !upgrade) return false;
      // Either an upgrade or a fresh window: this match sets the bar.
      r.rank = rank;
      r.last_report_ms = now;
      return true;
    }
    if (!r.used || (oldest->used && r.last_report_ms < oldest->last_report_ms)) oldest = &r;
  }
  oldest->used = true;
  memcpy(oldest->mac, m.mac, 6);
  oldest->rank = rank;
  oldest->last_report_ms = now;
  return true;
}

// ---- Reporting --------------------------------------------------------------

void report(const Match &m, int8_t rssi, uint8_t channel) {
  g_counters.matches++;
  const RuleInfo &info = rule_info(m.rule);
  const bool specific = m.strength == Strength::Specific;

  // Every match goes to the recorder; rate limiting only applies to logs and
  // indications.
  if (sighting_hooks[0]) {
    fy_detector_sighting s = {};
    memcpy(s.mac, m.mac, sizeof(s.mac));
    s.rule = static_cast<uint8_t>(m.rule);
    s.rank = rank_of(m);
    s.specific = specific;
    s.ble = info.transport == Transport::Ble;
    s.rssi = rssi;
    s.channel = s.ble ? 0 : channel;
    memcpy(s.name, m.name, sizeof(s.name));
    s.uptime_ms = now_ms();
    for (fy_detector_sighting_hook hook : sighting_hooks) {
      if (hook) hook(s);
    }
  }

  if (!should_report(m)) return;
  g_counters.reported++;
  char mac[18];
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m.mac[0], m.mac[1], m.mac[2],
           m.mac[3], m.mac[4], m.mac[5]);
  // Advertised names are untrusted bytes; logs never carry control sequences.
  char name[sizeof(m.name)];
  sanitize_name(m.name, name, sizeof(name));
  char where[8];
  if (info.transport == Transport::Ble) {
    snprintf(where, sizeof(where), "ble");
  } else {
    snprintf(where, sizeof(where), "ch%u", static_cast<unsigned>(channel));
  }

  // Fix state only: coordinates go to storage, not to the console.
  fy_gps_status gps;
  fy_gps_get_status(&gps);
  char loc[32];
  if (!gps.available) {
    snprintf(loc, sizeof(loc), "none");
  } else if (gps.position_current) {
    snprintf(loc, sizeof(loc), "current hdop=%.1f", gps.nav.hdop_valid ? gps.nav.hdop : 0.0f);
  } else {
    snprintf(loc, sizeof(loc), "no_fix");
  }

  ESP_LOGI(TAG, "match strength=%s rule=%s mac=%s rssi=%d via=%s name=\"%s\" gps=%s",
           specific ? "specific" : "possible", info.id, mac, rssi, where, name, loc);

#if CONFIG_FY_SERIAL_JSON
  // The same match as one line in the upstream flock-you format, for its
  // Flask dashboard. A single printf keeps the line whole between log lines
  // from other tasks.
  char json[384];
  if (format_dashboard_detection(m, rssi, channel, json, sizeof(json))) printf("%s\n", json);
#endif

  fy_detector_match_info mi = {};
  mi.valid = true;
  mi.rule_id = info.id;
  mi.specific = specific;
  mi.ble = info.transport == Transport::Ble;
  mi.rssi = rssi;
  memcpy(mi.mac, m.mac, 6);
  mi.at_ms = now_ms();
  portENTER_CRITICAL(&status_lock);
  last_match = mi;
  if (specific) {
    last_specific = mi;
    reported_specific++;
  }
  portEXIT_CRITICAL(&status_lock);

  if (specific) fy_status_led_indicate_match();
}

// ---- Tasks ------------------------------------------------------------------

void matcher_task(void *) {
  WifiCapture wifi;
  BleCapture ble;
  Match matches[kMaxWifiMatchesPerFrame];
  for (;;) {
    bool busy = true;
    while (busy) {
      busy = false;
      for (int i = 0; i < kBatch && xQueueReceive(wifi_queue, &wifi, 0) == pdTRUE; i++) {
        busy = true;
        const size_t n =
            match_wifi_frame(wifi.frame, wifi.len, wifi.is_mgmt, matches, kMaxWifiMatchesPerFrame);
        for (size_t k = 0; k < n; k++) report(matches[k], wifi.rssi, wifi.channel);
      }
      for (int i = 0; i < kBatch && xQueueReceive(ble_queue, &ble, 0) == pdTRUE; i++) {
        busy = true;
        Match m;
        if (match_ble_advert(ble.addr, ble.public_addr, ble.data, ble.len, &m)) {
          report(m, ble.rssi, 0);
        }
      }
    }
    // Woken by submit_*; the timeout is only a safety net.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}

void status_task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kStatusIntervalMs));
    ESP_LOGI(TAG,
             "status board=%s wifi=%s ble=%s ch=%u hops=%lu wifi_frames=%lu wifi_cand=%lu "
             "wifi_drop=%lu wifi_trunc=%lu ble_adv=%lu ble_drop=%lu ble_oversize=%lu "
             "matches=%lu reported=%lu heap=%lu heap_min=%lu",
             fy_board_name(), g_counters.wifi_capture.load() ? "on" : "paused",
             g_counters.ble_scan.load() ? "on" : "paused",
             static_cast<unsigned>(g_counters.channel.load()),
             static_cast<unsigned long>(g_counters.wifi_hops.load()),
             static_cast<unsigned long>(g_counters.wifi_frames.load()),
             static_cast<unsigned long>(g_counters.wifi_candidates.load()),
             static_cast<unsigned long>(g_counters.wifi_dropped.load()),
             static_cast<unsigned long>(g_counters.wifi_truncated.load()),
             static_cast<unsigned long>(g_counters.ble_adverts.load()),
             static_cast<unsigned long>(g_counters.ble_dropped.load()),
             static_cast<unsigned long>(g_counters.ble_oversize.load()),
             static_cast<unsigned long>(g_counters.matches.load()),
             static_cast<unsigned long>(g_counters.reported.load()),
             static_cast<unsigned long>(esp_get_free_heap_size()),
             static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
  }
}

bool submit(QueueHandle_t queue, const void *item, std::atomic<uint32_t> &dropped) {
  if (!queue || xQueueSend(queue, item, 0) != pdTRUE) {
    dropped++;
    return false;
  }
  if (matcher_handle) xTaskNotifyGive(matcher_handle);
  return true;
}

}  // namespace

bool submit_wifi(const WifiCapture &capture) {
  return submit(wifi_queue, &capture, g_counters.wifi_dropped);
}

bool submit_ble(const BleCapture &capture) {
  return submit(ble_queue, &capture, g_counters.ble_dropped);
}

}  // namespace fy::detector

using namespace fy::detector;

esp_err_t fy_detector_start() {
  fy::Options options;  // Kconfig booleans are only defined when on
#ifndef CONFIG_FY_SIG_MOTOROLA
  options.motorola = false;
#endif
#ifndef CONFIG_FY_SIG_SIERRA_WIRELESS
  options.sierra_wireless = false;
#endif
#ifdef CONFIG_FY_SIG_CRADLEPOINT
  options.cradlepoint = true;
#endif
#ifdef CONFIG_FY_SIG_RADIO_OUIS_ANY_FRAME
  options.radio_ouis_any_frame = true;
#endif
  fy::set_options(options);  // before capture starts

  wifi_queue = xQueueCreate(kWifiQueueDepth, sizeof(WifiCapture));
  ble_queue = xQueueCreate(kBleQueueDepth, sizeof(BleCapture));
  if (!wifi_queue || !ble_queue) return ESP_ERR_NO_MEM;

  if (xTaskCreate(matcher_task, "fy_match", 6144, nullptr, 4, &matcher_handle) != pdPASS ||
      xTaskCreate(status_task, "fy_status", 3072, nullptr, 1, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG,
           "signature set: %u watchlist OUIs, %u Flock radio OUIs (%s), %u rules; "
           "motorola=%d sierra=%d cradlepoint=%d",
           static_cast<unsigned>(fy::watchlist_count()),
           static_cast<unsigned>(fy::flock_radio_oui_count()),
           options.radio_ouis_any_frame ? "any frame" : "probes only",
           static_cast<unsigned>(fy::Rule::Count), options.motorola, options.sierra_wireless,
           options.cradlepoint);
  ESP_RETURN_ON_ERROR(wifi_capture_start(), TAG, "Wi-Fi capture");
  ESP_RETURN_ON_ERROR(ble_capture_start(), TAG, "BLE capture");
  return ESP_OK;
}

void fy_detector_set_wifi_capture(bool enabled) { wifi_capture_set_enabled(enabled); }

void fy_detector_set_ble_scan(bool enabled) { ble_capture_set_enabled(enabled); }

bool fy_detector_add_sighting_hook(fy_detector_sighting_hook hook) {
  for (fy_detector_sighting_hook &slot : sighting_hooks) {
    if (!slot) {
      slot = hook;
      return true;
    }
  }
  return false;
}

void fy_detector_get_status(fy_detector_status *out) {
  out->wifi_capture = g_counters.wifi_capture.load();
  out->ble_scan = g_counters.ble_scan.load();
  out->channel = g_counters.channel.load();
  out->wifi_frames = g_counters.wifi_frames.load();
  out->ble_adverts = g_counters.ble_adverts.load();
  out->reported = g_counters.reported.load();
  portENTER_CRITICAL(&status_lock);
  out->reported_specific = reported_specific;
  out->last = last_match;
  out->last_specific = last_specific;
  portEXIT_CRITICAL(&status_lock);
}
