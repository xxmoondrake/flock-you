// SPDX-License-Identifier: MIT
//
// Host unit tests for the status LED patterns: pio test -e native

#include <unity.h>

#include "fy_status_pattern.h"

using namespace fy::status;

void setUp() {}
void tearDown() {}

// Counts the times the LED turns on during the first `window_ms` of a mode.
static int blinks(Mode mode, uint32_t window_ms) {
  int count = 0;
  bool was_on = false;
  for (uint32_t t = 0; t < window_ms;) {
    const LedState s = mode_state(mode, t);
    if (s.on && !was_on) count++;
    was_on = s.on;
    if (s.hold_ms == 0) break;
    t += s.hold_ms;
  }
  return count;
}

void test_booting_is_solid() {
  const uint32_t times[] = {0, 1500, 60000};
  for (uint32_t t : times) {
    const LedState s = mode_state(Mode::Booting, t);
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_EQUAL_UINT32(0, s.hold_ms);
  }
}

void test_scanning_blinks_every_3_s() {
  LedState s = mode_state(Mode::Scanning, 0);
  TEST_ASSERT_TRUE(s.on);
  TEST_ASSERT_EQUAL_UINT32(100, s.hold_ms);
  s = mode_state(Mode::Scanning, 40);
  TEST_ASSERT_TRUE(s.on);
  TEST_ASSERT_EQUAL_UINT32(60, s.hold_ms);
  s = mode_state(Mode::Scanning, 100);
  TEST_ASSERT_FALSE(s.on);
  TEST_ASSERT_EQUAL_UINT32(2900, s.hold_ms);
  TEST_ASSERT_TRUE(mode_state(Mode::Scanning, 3000).on);
  TEST_ASSERT_EQUAL(4, blinks(Mode::Scanning, 12000));
}

void test_standby_blinks_less_often() {
  TEST_ASSERT_TRUE(mode_state(Mode::Standby, 0).on);
  const LedState s = mode_state(Mode::Standby, 100);
  TEST_ASSERT_FALSE(s.on);
  TEST_ASSERT_EQUAL_UINT32(9900, s.hold_ms);
  TEST_ASSERT_TRUE(mode_state(Mode::Standby, 10000).on);
  TEST_ASSERT_EQUAL(3, blinks(Mode::Standby, 30000));
  TEST_ASSERT_TRUE(blinks(Mode::Standby, 30000) < blinks(Mode::Scanning, 30000));
}

void test_dashboard_double_blinks_at_scanning_cadence() {
  TEST_ASSERT_TRUE(mode_state(Mode::Dashboard, 0).on);
  TEST_ASSERT_FALSE(mode_state(Mode::Dashboard, 100).on);
  TEST_ASSERT_TRUE(mode_state(Mode::Dashboard, 250).on);
  const LedState gap = mode_state(Mode::Dashboard, 350);
  TEST_ASSERT_FALSE(gap.on);
  TEST_ASSERT_EQUAL_UINT32(2650, gap.hold_ms);
  // Each 3 s scanning blink becomes a pair.
  TEST_ASSERT_TRUE(mode_state(Mode::Dashboard, 3000).on);
  TEST_ASSERT_EQUAL(2 * blinks(Mode::Scanning, 12000), blinks(Mode::Dashboard, 12000));
}

void test_match_burst_is_three_quick_flashes() {
  int flashes = 0;
  bool was_on = false;
  for (uint32_t t = 0; t < kMatchBurstMs;) {
    const LedState s = match_state(t);
    TEST_ASSERT_TRUE(s.hold_ms > 0);
    if (s.on && !was_on) flashes++;
    was_on = s.on;
    t += s.hold_ms;
  }
  TEST_ASSERT_EQUAL(3, flashes);
  const LedState after = match_state(kMatchBurstMs);
  TEST_ASSERT_FALSE(after.on);
  TEST_ASSERT_EQUAL_UINT32(0, after.hold_ms);
}

void test_patterns_repeat_after_long_uptime() {
  // About 49.7 days of milliseconds wraps a uint32_t; patterns stay periodic
  // up to then.
  const uint32_t days_40 = 40u * 24 * 3600 * 1000;
  const uint32_t aligned = days_40 - days_40 % 30000;  // a multiple of both periods
  TEST_ASSERT_TRUE(mode_state(Mode::Scanning, aligned).on);
  TEST_ASSERT_TRUE(mode_state(Mode::Standby, aligned).on);
  TEST_ASSERT_FALSE(mode_state(Mode::Dashboard, aligned + 150).on);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_booting_is_solid);
  RUN_TEST(test_scanning_blinks_every_3_s);
  RUN_TEST(test_standby_blinks_less_often);
  RUN_TEST(test_dashboard_double_blinks_at_scanning_cadence);
  RUN_TEST(test_match_burst_is_three_quick_flashes);
  RUN_TEST(test_patterns_repeat_after_long_uptime);
  return UNITY_END();
}
