// SPDX-License-Identifier: MIT
//
// Host unit tests for single-button navigation: pio test -e native

#include <unity.h>

#include "fy_ui_controller.h"

using namespace fy::ui;

void setUp() {}
void tearDown() {}

void test_press_classification() {
  TEST_ASSERT_EQUAL(Press::Short, classify_press(100, 60000));
  TEST_ASSERT_EQUAL(Press::Long, classify_press(600, 60000));
  TEST_ASSERT_EQUAL(Press::PowerOff, classify_press(4200, 60000));
  // Power-off only more than 30 s after boot, as in Meshtastic.
  TEST_ASSERT_EQUAL(Press::Long, classify_press(4200, 10000));
}

void test_short_press_cycles_screens() {
  UiState s;
  TEST_ASSERT_EQUAL(Screen::Home, s.screen);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Screen::Detections, s.screen);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Screen::Data, s.screen);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Screen::Device, s.screen);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Screen::Home, s.screen);
}

void test_hold_without_menu_shows_notice() {
  UiState s;
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 1000));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
  TEST_ASSERT_TRUE(message_active(s, 1100));
  TEST_ASSERT_FALSE(message_active(s, 5000));
}

void test_data_menu_navigation_and_save() {
  UiState s;
  s.screen = Screen::Data;
  handle_press(s, Press::Long, 0);
  TEST_ASSERT_EQUAL(Overlay::DataMenu, s.overlay);
  TEST_ASSERT_EQUAL_UINT8(0, s.item);
  TEST_ASSERT_EQUAL(Action::SaveOpen, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
  TEST_ASSERT_EQUAL(Screen::Data, s.screen);
}

void test_menu_items_wrap_and_back_closes() {
  UiState s;
  s.screen = Screen::Data;
  handle_press(s, Press::Long, 0);
  size_t n = 0;
  menu_items(Overlay::DataMenu, &n);
  for (size_t i = 0; i < n; i++) handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL_UINT8(0, s.item);  // wrapped
  handle_press(s, Press::Short, 0);
  handle_press(s, Press::Short, 0);    // "Back"
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
}

void test_erase_requires_moving_off_cancel() {
  UiState s;
  s.screen = Screen::Data;
  handle_press(s, Press::Long, 0);   // open menu
  handle_press(s, Press::Short, 0);  // "Erase all records"
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::EraseConfirm, s.overlay);
  TEST_ASSERT_EQUAL_UINT8(0, s.item);  // "Cancel" highlighted first

  // Selecting straight away cancels.
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);

  // Moving to "Erase everything" and selecting erases.
  handle_press(s, Press::Long, 0);
  handle_press(s, Press::Short, 0);
  handle_press(s, Press::Long, 0);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Action::EraseAll, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
}

void test_power_off_from_anywhere() {
  UiState s;
  s.screen = Screen::Data;
  s.overlay = Overlay::EraseConfirm;
  TEST_ASSERT_EQUAL(Action::PowerOff, handle_press(s, Press::PowerOff, 0));
}

void test_device_menu_selects_modes_and_dashboard() {
  size_t n = 0;
  menu_items(Overlay::DeviceMenu, &n);
  TEST_ASSERT_EQUAL_UINT(4, n);

  const Action expected[] = {Action::SelectScanning, Action::SelectStandby,
                             Action::ToggleDashboard, Action::None};
  for (size_t item = 0; item < n; item++) {
    UiState s;
    s.screen = Screen::Device;
    handle_press(s, Press::Long, 0);
    TEST_ASSERT_EQUAL(Overlay::DeviceMenu, s.overlay);
    TEST_ASSERT_EQUAL_UINT8(0, s.item);
    for (size_t i = 0; i < item; i++) handle_press(s, Press::Short, 0);
    TEST_ASSERT_EQUAL(expected[item], handle_press(s, Press::Long, 0));
    TEST_ASSERT_EQUAL(Overlay::None, s.overlay);  // every choice closes the menu
    TEST_ASSERT_EQUAL(Screen::Device, s.screen);
  }
}

void test_dashboard_is_separate_from_mode_items() {
  size_t n = 0;
  const char *const *items = menu_items(Overlay::DeviceMenu, &n);
  TEST_ASSERT_EQUAL_STRING("Scanning", items[0]);
  TEST_ASSERT_EQUAL_STRING("Standby", items[1]);
  TEST_ASSERT_EQUAL_STRING("WiFi dashboard", items[2]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_press_classification);
  RUN_TEST(test_short_press_cycles_screens);
  RUN_TEST(test_hold_without_menu_shows_notice);
  RUN_TEST(test_data_menu_navigation_and_save);
  RUN_TEST(test_menu_items_wrap_and_back_closes);
  RUN_TEST(test_erase_requires_moving_off_cancel);
  RUN_TEST(test_power_off_from_anywhere);
  RUN_TEST(test_device_menu_selects_modes_and_dashboard);
  RUN_TEST(test_dashboard_is_separate_from_mode_items);
  return UNITY_END();
}
