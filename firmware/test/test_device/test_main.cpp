// SPDX-License-Identifier: MIT
//
// Host unit tests for device modes and dashboard session timing:
// pio test -e native

#include <unity.h>

#include "fy_device_mode.h"

using namespace fy::device;

void setUp() {}
void tearDown() {}

constexpr uint32_t kIdle = 600000;  // 10 minutes

void assert_outputs(const State &s, bool wifi, bool ble, bool ap) {
  const Outputs o = outputs(s);
  TEST_ASSERT_EQUAL(wifi, o.wifi_capture);
  TEST_ASSERT_EQUAL(ble, o.ble_scan);
  TEST_ASSERT_EQUAL(ap, o.access_point);
}

void test_boot_state_is_scanning() {
  State s;
  TEST_ASSERT_EQUAL(Mode::Scanning, s.selected);
  TEST_ASSERT_FALSE(s.dashboard);
  assert_outputs(s, true, true, false);
}

void test_standby_stops_all_detection() {
  State s;
  select_mode(s, Mode::Standby);
  assert_outputs(s, false, false, false);
  select_mode(s, Mode::Scanning);
  assert_outputs(s, true, true, false);
}

void test_dashboard_pauses_detection_and_restores_mode() {
  State s;
  start_dashboard(s, 1000);
  assert_outputs(s, false, false, true);
  stop_dashboard(s);
  assert_outputs(s, true, true, false);

  select_mode(s, Mode::Standby);
  start_dashboard(s, 2000);
  assert_outputs(s, false, false, true);
  stop_dashboard(s);
  assert_outputs(s, false, false, false);  // back to standby, not scanning
}

void test_selection_during_session_applies_after() {
  State s;
  start_dashboard(s, 0);
  select_mode(s, Mode::Standby);
  assert_outputs(s, false, false, true);  // still the dashboard
  stop_dashboard(s);
  assert_outputs(s, false, false, false);
}

void test_session_expires_after_inactivity() {
  State s;
  TEST_ASSERT_EQUAL_UINT32(0, dashboard_remaining_ms(s, 5000, kIdle));  // no session
  start_dashboard(s, 1000);
  TEST_ASSERT_EQUAL_UINT32(kIdle, dashboard_remaining_ms(s, 1000, kIdle));
  TEST_ASSERT_EQUAL_UINT32(kIdle - 60000, dashboard_remaining_ms(s, 61000, kIdle));
  TEST_ASSERT_EQUAL_UINT32(0, dashboard_remaining_ms(s, 1000 + kIdle, kIdle));

  // Interaction restarts the countdown.
  note_activity(s, 500000);
  TEST_ASSERT_EQUAL_UINT32(kIdle, dashboard_remaining_ms(s, 500000, kIdle));
  TEST_ASSERT_TRUE(dashboard_remaining_ms(s, 1000 + kIdle, kIdle) > 0);
}

void test_activity_without_session_is_ignored() {
  State s;
  note_activity(s, 123);
  TEST_ASSERT_EQUAL_UINT32(0, s.last_activity_ms);
}

void test_countdown_survives_uptime_wrap() {
  State s;
  start_dashboard(s, 0xfffff000u);
  TEST_ASSERT_EQUAL_UINT32(kIdle - 0x2000, dashboard_remaining_ms(s, 0x00001000u, kIdle));
}

// Five short presses at `gap_ms` intervals, each held `hold_ms`; returns the
// press number that triggered, or 0.
int press_series(PressCounter &c, uint32_t start_ms, int presses, uint32_t gap_ms,
                 uint32_t hold_ms) {
  for (int i = 0; i < presses; i++) {
    if (count_press(c, start_ms + i * gap_ms, hold_ms)) return i + 1;
  }
  return 0;
}

void test_five_quick_presses_trigger() {
  PressCounter c;
  TEST_ASSERT_EQUAL_INT(5, press_series(c, 1000, 5, 400, 120));
  // The count starts over: the next four do not trigger, the fifth does.
  TEST_ASSERT_EQUAL_INT(0, press_series(c, 5000, 4, 400, 120));
  TEST_ASSERT_TRUE(count_press(c, 5000 + 4 * 400, 120));
}

void test_slow_presses_start_over() {
  PressCounter c;
  TEST_ASSERT_EQUAL_INT(0, press_series(c, 0, 4, 400, 120));
  // A pause longer than the gap limit restarts the count at this press.
  TEST_ASSERT_FALSE(count_press(c, 1200 + kActivationMaxGapMs + 1, 120));
  TEST_ASSERT_EQUAL_UINT8(1, c.count);
  TEST_ASSERT_EQUAL_INT(0, press_series(c, 10000, 5, kActivationMaxGapMs + 50, 120));
}

void test_long_hold_resets_count() {
  PressCounter c;
  press_series(c, 0, 3, 400, 120);
  TEST_ASSERT_FALSE(count_press(c, 1600, kActivationMaxHoldMs + 1));
  TEST_ASSERT_EQUAL_UINT8(0, c.count);
  TEST_ASSERT_EQUAL_INT(5, press_series(c, 3000, 5, 400, 120));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_five_quick_presses_trigger);
  RUN_TEST(test_slow_presses_start_over);
  RUN_TEST(test_long_hold_resets_count);
  RUN_TEST(test_boot_state_is_scanning);
  RUN_TEST(test_standby_stops_all_detection);
  RUN_TEST(test_dashboard_pauses_detection_and_restores_mode);
  RUN_TEST(test_selection_during_session_applies_after);
  RUN_TEST(test_session_expires_after_inactivity);
  RUN_TEST(test_activity_without_session_is_ignored);
  RUN_TEST(test_countdown_survives_uptime_wrap);
  return UNITY_END();
}
