// SPDX-License-Identifier: MIT

#include "fy_alerts.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "fy_board.h"
#include "fy_detector.h"
#include "fy_signatures.h"

namespace {

using fy::alerts::Settings;

const char *TAG = "alerts";

constexpr char kNvsNamespace[] = "fy_alerts";
constexpr char kSettingsKey[] = "settings";
constexpr uint8_t kTestTier = 4;  // a test plays the most confident tier's sound
constexpr int kQueueLength = 16;  // sightings arriving while a sound plays

enum class Kind : uint8_t { Sighting, Test };

struct Event {
  Kind kind;
  uint8_t tier;
  uint8_t mac[6];
  uint32_t at_ms;
};

std::atomic<uint8_t> packed{fy::alerts::pack(Settings{})};
std::atomic<uint32_t> tests{0};
QueueHandle_t events = nullptr;  // only with a buzzer

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Runs in the matcher task: hand the sighting over and return. A full queue
// drops it, which at worst delays a sound to the address's next sighting.
void on_sighting(const fy_detector_sighting &s) {
  Event e = {Kind::Sighting, fy::rule_info(static_cast<fy::Rule>(s.rule)).upstream_tier, {},
             s.uptime_ms};
  memcpy(e.mac, s.mac, sizeof(e.mac));
  xQueueSend(events, &e, 0);
}

void play(const fy::alerts::Sound &sound) {
  for (uint8_t i = 0; i < sound.count; i++) {
    fy_board_buzzer_tone(sound.notes[i].hz);
    vTaskDelay(pdMS_TO_TICKS(sound.notes[i].ms));
  }
  fy_board_buzzer_tone(0);
}

void alerts_task(void *) {
  if (fy_alerts_settings().buzzer_on()) play(fy::alerts::startup_sound());
  fy::alerts::Cadence cadence;
  for (;;) {
    Event e;
    const bool got = xQueueReceive(events, &e, pdMS_TO_TICKS(100)) == pdTRUE;
    const Settings s = fy_alerts_settings();
    if (got && e.kind == Kind::Test) {
      if (s.buzzer_on()) play(fy::alerts::tier_sound(kTestTier));
    } else if (got && cadence.on_sighting(e.mac, e.tier, e.at_ms) && s.buzzer_on() &&
               s.tier_audible(e.tier)) {
      play(fy::alerts::tier_sound(e.tier));
    }
    uint8_t tier = 0;
    const uint32_t now = now_ms();
    if (cadence.heartbeat_due(now, &tier) && s.buzzer_on() && s.tier_audible(tier)) {
      play(fy::alerts::heartbeat_sound());
      cadence.heartbeat_played(now);
    }
  }
}

}  // namespace

esp_err_t fy_alerts_start() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
    uint8_t bits = 0;
    if (nvs_get_u8(h, kSettingsKey, &bits) == ESP_OK) packed = bits;
    nvs_close(h);
  }
  const Settings s = fy_alerts_settings();
  if (!fy_board_has_buzzer()) {
    ESP_LOGI(TAG, "no buzzer; alerts %s", s.enabled ? "on" : "off");
    return ESP_OK;
  }
  fy_board_buzzer_tone(0);  // silent until a sound plays
  events = xQueueCreate(kQueueLength, sizeof(Event));
  if (!events) return ESP_ERR_NO_MEM;
  if (!fy_detector_add_sighting_hook(on_sighting)) return ESP_ERR_NO_MEM;
  if (xTaskCreate(alerts_task, "fy_alerts", 3072, nullptr, 2, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "buzzer on GPIO%d; alerts %s, buzzer %s, tiers 0x%02x", CONFIG_FY_BUZZER_GPIO,
           s.enabled ? "on" : "off", s.buzzer ? "on" : "off", s.tiers);
  return ESP_OK;
}

Settings fy_alerts_settings() { return fy::alerts::unpack(packed); }

bool fy_alerts_set(const Settings &settings) {
  const uint8_t bits = fy::alerts::pack(settings);
  packed = bits;
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_u8(h, kSettingsKey, bits) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

void fy_alerts_test() {
  tests++;
  if (!events) return;
  Event e = {Kind::Test, kTestTier, {}, now_ms()};
  xQueueSend(events, &e, 0);
}

uint32_t fy_alerts_tests() { return tests; }
