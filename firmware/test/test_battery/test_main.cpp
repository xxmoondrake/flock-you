// SPDX-License-Identifier: MIT
//
// Host unit tests for the battery charge estimate: pio test -e native

#include <unity.h>

#include "fy_battery_curve.h"

using fy::battery::percent_from_mv;

void test_reference_endpoints_and_clamping() {
  TEST_ASSERT_EQUAL_UINT8(100, percent_from_mv(4200));
  TEST_ASSERT_EQUAL_UINT8(100, percent_from_mv(4350));  // charging or above full
  TEST_ASSERT_EQUAL_UINT8(0, percent_from_mv(3300));
  TEST_ASSERT_EQUAL_UINT8(0, percent_from_mv(3000));
}

void test_reference_curve_points() {
  TEST_ASSERT_EQUAL_UINT8(50, percent_from_mv(3840));
  TEST_ASSERT_EQUAL_UINT8(80, percent_from_mv(4020));
  TEST_ASSERT_EQUAL_UINT8(10, percent_from_mv(3690));
}

void test_interpolates_between_points() {
  // Halfway between 3610 mV (5%) and 3690 mV (10%).
  const uint8_t p = percent_from_mv(3650);
  TEST_ASSERT_TRUE(p >= 7 && p <= 8);
}

void test_monotonic_over_range() {
  uint8_t prev = 0;
  for (uint32_t mv = 3300; mv <= 4200; mv += 5) {
    const uint8_t p = percent_from_mv(mv);
    TEST_ASSERT_TRUE_MESSAGE(p >= prev, "percentage must not drop as voltage rises");
    prev = p;
  }
}

void test_tuned_endpoints_rescale_curve() {
  // A battery that reads 4.10 V when full and should count as empty at 3.40 V.
  TEST_ASSERT_EQUAL_UINT8(100, percent_from_mv(4100, 3400, 4100));
  TEST_ASSERT_EQUAL_UINT8(0, percent_from_mv(3400, 3400, 4100));
  // The midpoint of the tuned range maps to the reference midpoint (3750 mV).
  TEST_ASSERT_EQUAL_UINT8(percent_from_mv(3750), percent_from_mv(3750, 3400, 4100));
}

void test_invalid_endpoints() {
  TEST_ASSERT_EQUAL_UINT8(0, percent_from_mv(3800, 4200, 4200));
  TEST_ASSERT_EQUAL_UINT8(0, percent_from_mv(3800, 4200, 3300));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reference_endpoints_and_clamping);
  RUN_TEST(test_reference_curve_points);
  RUN_TEST(test_interpolates_between_points);
  RUN_TEST(test_monotonic_over_range);
  RUN_TEST(test_tuned_endpoints_rescale_curve);
  RUN_TEST(test_invalid_endpoints);
  return UNITY_END();
}
