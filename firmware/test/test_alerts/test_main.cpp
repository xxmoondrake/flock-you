// SPDX-License-Identifier: MIT
//
// Host unit tests for the alert preferences and upstream's buzzer sounds
// and cadence: pio test -e native

#include <unity.h>

#include "fy_alerts_logic.h"

using namespace fy::alerts;

namespace {

uint32_t total_ms(const Sound &s) {
  uint32_t ms = 0;
  for (uint8_t i = 0; i < s.count; i++) ms += s.notes[i].ms;
  return ms;
}

const uint8_t kA[6] = {1, 2, 3, 4, 5, 6};
const uint8_t kB[6] = {1, 2, 3, 4, 5, 7};

}  // namespace

void setUp() {}
void tearDown() {}

void test_settings_default_on_and_round_trip() {
  Settings s;
  TEST_ASSERT_TRUE(s.enabled && s.buzzer && s.flash);
  TEST_ASSERT_EQUAL_HEX8(0x1F, s.tiers);  // every tier sounds, as upstream
  s.flash = false;
  s.tiers = 0x12;
  const Settings back = unpack(pack(s));
  TEST_ASSERT_TRUE(back.enabled);
  TEST_ASSERT_TRUE(back.buzzer);
  TEST_ASSERT_FALSE(back.flash);
  TEST_ASSERT_EQUAL_HEX8(0x12, back.tiers);
  TEST_ASSERT_EQUAL_HEX8(0xFF, pack(Settings{}));
}

void test_master_switch_keeps_the_outputs_settings() {
  Settings s;
  s.enabled = false;
  TEST_ASSERT_FALSE(s.buzzer_on());
  TEST_ASSERT_FALSE(s.flash_on());
  TEST_ASSERT_TRUE(s.buzzer && s.flash);
  s.enabled = true;
  s.buzzer = false;
  TEST_ASSERT_FALSE(s.buzzer_on());
  TEST_ASSERT_TRUE(s.flash_on());
}

void test_tier_mask() {
  Settings s;
  s.tiers = 0x10;  // only tier 4
  TEST_ASSERT_TRUE(s.tier_audible(4));
  for (uint8_t t = 0; t < 4; t++) TEST_ASSERT_FALSE(s.tier_audible(t));
  TEST_ASSERT_FALSE(s.tier_audible(5));
}

void test_sounds_match_upstream() {
  const Sound t4 = tier_sound(4);
  TEST_ASSERT_EQUAL_UINT8(3, t4.count);  // rising pair with a gap
  TEST_ASSERT_EQUAL_UINT16(2000, t4.notes[0].hz);
  TEST_ASSERT_EQUAL_UINT16(0, t4.notes[1].hz);
  TEST_ASSERT_EQUAL_UINT16(2800, t4.notes[2].hz);
  TEST_ASSERT_EQUAL_UINT32(55 + 25 + 55, total_ms(t4));
  const Sound t3 = tier_sound(3);
  TEST_ASSERT_EQUAL_UINT16(1400, t3.notes[0].hz);
  TEST_ASSERT_EQUAL_UINT16(1800, t3.notes[2].hz);
  const uint16_t blips[] = {600, 800, 1200};
  for (uint8_t t = 0; t < 3; t++) {
    const Sound s = tier_sound(t);
    TEST_ASSERT_EQUAL_UINT8(1, s.count);
    TEST_ASSERT_EQUAL_UINT16(blips[t], s.notes[0].hz);
    TEST_ASSERT_EQUAL_UINT16(45, s.notes[0].ms);
  }
  const Sound hb = heartbeat_sound();
  TEST_ASSERT_EQUAL_UINT8(3, hb.count);
  TEST_ASSERT_EQUAL_UINT16(1500, hb.notes[0].hz);
  TEST_ASSERT_EQUAL_UINT16(1500, hb.notes[2].hz);
  TEST_ASSERT_EQUAL_UINT32(70 * 3, total_ms(hb));
  const Sound tune = startup_sound();
  const uint16_t notes[] = {262, 523, 220, 440, 233, 466};
  TEST_ASSERT_EQUAL_UINT8(11, tune.count);
  for (int i = 0; i < 6; i++) TEST_ASSERT_EQUAL_UINT16(notes[i], tune.notes[2 * i].hz);
  TEST_ASSERT_EQUAL_UINT16(160, tune.notes[10].ms);
  TEST_ASSERT_EQUAL_UINT32(5 * 95 + 160 + 5 * 22, total_ms(tune));
}

void test_new_address_sounds_once_then_again_after_absence() {
  Cadence c;
  TEST_ASSERT_TRUE(c.on_sighting(kA, 2, 1000));
  TEST_ASSERT_FALSE(c.on_sighting(kA, 2, 1500));  // the same device again
  TEST_ASSERT_TRUE(c.on_sighting(kB, 1, 1600));   // another device
  // Back after more than kRediscoverMs of silence.
  TEST_ASSERT_FALSE(c.on_sighting(kA, 2, 1500 + kRediscoverMs));
  TEST_ASSERT_TRUE(c.on_sighting(kA, 2, 1500 + 2 * kRediscoverMs + 1));
}

void test_a_higher_tier_sounds_again() {
  Cadence c;
  TEST_ASSERT_TRUE(c.on_sighting(kA, 1, 1000));
  TEST_ASSERT_TRUE(c.on_sighting(kA, 4, 1100));  // confirmed: the better sound
  TEST_ASSERT_FALSE(c.on_sighting(kA, 2, 1200));  // weaker again: silent
}

void test_heartbeat_cadence() {
  Cadence c;
  uint8_t tier = 0;
  TEST_ASSERT_FALSE(c.heartbeat_due(0, &tier));  // nothing seen yet
  c.on_sighting(kA, 3, 1000);
  // Still in range, but the first heartbeat waits a full interval after the
  // sound.
  for (uint32_t t = 1000; t < 1000 + kHeartbeatMs; t += 1000) {
    c.on_sighting(kA, 3, t);
    TEST_ASSERT_FALSE(c.heartbeat_due(t, &tier));
  }
  c.on_sighting(kA, 3, 1000 + kHeartbeatMs);
  TEST_ASSERT_TRUE(c.heartbeat_due(1000 + kHeartbeatMs, &tier));
  TEST_ASSERT_EQUAL_UINT8(3, tier);
  c.heartbeat_played(1000 + kHeartbeatMs);
  TEST_ASSERT_FALSE(c.heartbeat_due(1001 + kHeartbeatMs, &tier));
  // Out of range: no heartbeat however long it has been.
  TEST_ASSERT_FALSE(c.heartbeat_due(1000 + kHeartbeatMs + kActiveMs + 1 + kHeartbeatMs, &tier));
}

void test_heartbeat_voice_is_the_best_tier_in_range() {
  Cadence c;
  uint8_t tier = 0;
  c.on_sighting(kA, 1, 1000);
  c.on_sighting(kB, 4, 1100);
  for (uint32_t t = 3000; t <= 1100 + kHeartbeatMs; t += 2000) c.on_sighting(kA, 1, t);
  TEST_ASSERT_TRUE(c.heartbeat_due(1100 + kHeartbeatMs, &tier));
  TEST_ASSERT_EQUAL_UINT8(4, tier);
  // After the range empties, the next sighting starts a fresh voice.
  const uint32_t later = 1100 + kHeartbeatMs + kActiveMs + 1000;
  c.on_sighting(kA, 1, later);
  c.on_sighting(kA, 1, later + kHeartbeatMs);
  TEST_ASSERT_TRUE(c.heartbeat_due(later + kHeartbeatMs, &tier));
  TEST_ASSERT_EQUAL_UINT8(1, tier);
}

void test_full_table_forgets_the_longest_unseen() {
  Cadence c;
  uint8_t mac[6] = {9, 9, 9, 9, 0, 0};
  for (uint8_t i = 0; i < 33; i++) {  // one more than the table holds
    mac[5] = i;
    TEST_ASSERT_TRUE(c.on_sighting(mac, 2, 100 + i));
  }
  mac[5] = 0;  // pushed out: new again
  TEST_ASSERT_TRUE(c.on_sighting(mac, 2, 200));
  mac[5] = 32;  // still remembered
  TEST_ASSERT_FALSE(c.on_sighting(mac, 2, 200));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_settings_default_on_and_round_trip);
  RUN_TEST(test_master_switch_keeps_the_outputs_settings);
  RUN_TEST(test_tier_mask);
  RUN_TEST(test_sounds_match_upstream);
  RUN_TEST(test_new_address_sounds_once_then_again_after_absence);
  RUN_TEST(test_a_higher_tier_sounds_again);
  RUN_TEST(test_heartbeat_cadence);
  RUN_TEST(test_heartbeat_voice_is_the_best_tier_in_range);
  RUN_TEST(test_full_table_forgets_the_longest_unseen);
  return UNITY_END();
}
