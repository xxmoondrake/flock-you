// SPDX-License-Identifier: MIT
//
// Host unit tests for single-button navigation: pio test -e native

#include <unity.h>

#include "fy_ui_alerts.h"
#include "fy_ui_controller.h"

using namespace fy::ui;

void setUp() {}
void tearDown() {}

void test_press_classification() {
  TEST_ASSERT_EQUAL(Press::Short, classify_press(100, 60000));
  TEST_ASSERT_EQUAL(Press::Long, classify_press(600, 60000));
  TEST_ASSERT_EQUAL(Press::PowerOff, classify_press(4200, 60000));
  // Power-off only more than 30 s after boot, as in Meshtastic; earlier, the
  // hold is recognised but only explained.
  TEST_ASSERT_EQUAL(Press::PowerOffEarly, classify_press(4200, 10000));
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

void test_early_power_off_hold_only_explains() {
  UiState s;
  s.screen = Screen::Device;
  s.overlay = Overlay::DeviceMenu;
  s.item = 0;  // Scanning, which a long press would select
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::PowerOffEarly, 1000));
  TEST_ASSERT_EQUAL(Overlay::DeviceMenu, s.overlay);
  TEST_ASSERT_EQUAL_STRING("Power off after 30 s", s.message);
  TEST_ASSERT_TRUE(message_active(s, 1000));
}

// Opens the Device menu and moves to `item`.
UiState device_menu_at(size_t item, bool dashboard_running = false) {
  UiState s;
  s.screen = Screen::Device;
  s.dashboard_running = dashboard_running;
  handle_press(s, Press::Long, 0);
  for (size_t i = 0; i < item; i++) handle_press(s, Press::Short, 0);
  return s;
}

void test_device_menu_selects_modes() {
  size_t n = 0;
  const char *const *items = menu_items(Overlay::DeviceMenu, &n);
  TEST_ASSERT_EQUAL_UINT(5, n);
  TEST_ASSERT_EQUAL_STRING("Scanning", items[0]);
  TEST_ASSERT_EQUAL_STRING("Standby", items[1]);
  TEST_ASSERT_EQUAL_STRING("Dashboard", items[2]);
  TEST_ASSERT_EQUAL_STRING("Alerts", items[3]);

  UiState s = device_menu_at(0);
  TEST_ASSERT_EQUAL(Action::SelectScanning, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
  s = device_menu_at(1);
  TEST_ASSERT_EQUAL(Action::SelectStandby, handle_press(s, Press::Long, 0));
  s = device_menu_at(4);  // Back
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
}

void test_start_dashboard_offers_usb_and_wifi() {
  size_t n = 0;
  const char *const *items = menu_items(Overlay::DashboardMenu, &n);
  TEST_ASSERT_EQUAL_UINT(3, n);
  TEST_ASSERT_EQUAL_STRING("USB dashboard", items[0]);
  TEST_ASSERT_EQUAL_STRING("WiFi dashboard", items[1]);

  UiState s = device_menu_at(2);
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::DashboardMenu, s.overlay);
  TEST_ASSERT_EQUAL_UINT8(0, s.item);
  TEST_ASSERT_EQUAL(Action::StartUsbDashboard, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);

  s = device_menu_at(2);
  handle_press(s, Press::Long, 0);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Action::StartWifiDashboard, handle_press(s, Press::Long, 0));

  // Back returns to the Device menu on its dashboard item.
  s = device_menu_at(2);
  handle_press(s, Press::Long, 0);
  handle_press(s, Press::Short, 0);
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::DeviceMenu, s.overlay);
  TEST_ASSERT_EQUAL_UINT8(2, s.item);
}

void test_running_dashboard_is_stopped_directly() {
  UiState s = device_menu_at(2, true);
  TEST_ASSERT_EQUAL(Action::StopDashboard, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::None, s.overlay);
}

void test_alerts_menu_toggles_in_place_and_returns() {
  size_t n = 0;
  const char *const *items = menu_items(Overlay::AlertsMenu, &n);
  TEST_ASSERT_EQUAL_UINT(5, n);
  TEST_ASSERT_EQUAL_STRING("Alerts", items[0]);
  TEST_ASSERT_EQUAL_STRING("Buzzer", items[1]);
  TEST_ASSERT_EQUAL_STRING("Screen flash", items[2]);
  TEST_ASSERT_EQUAL_STRING("Test alert", items[3]);

  UiState s = device_menu_at(3);
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::AlertsMenu, s.overlay);
  TEST_ASSERT_EQUAL_UINT8(0, s.item);
  // Each setting changes in place; the menu stays open to show it.
  TEST_ASSERT_EQUAL(Action::ToggleAlerts, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::AlertsMenu, s.overlay);
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Short, 0));
  TEST_ASSERT_EQUAL(Action::ToggleBuzzer, handle_press(s, Press::Long, 0));
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Action::ToggleFlash, handle_press(s, Press::Long, 0));
  handle_press(s, Press::Short, 0);
  TEST_ASSERT_EQUAL(Action::TestAlert, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::AlertsMenu, s.overlay);
  handle_press(s, Press::Short, 0);  // Back
  TEST_ASSERT_EQUAL(Action::None, handle_press(s, Press::Long, 0));
  TEST_ASSERT_EQUAL(Overlay::DeviceMenu, s.overlay);
  TEST_ASSERT_EQUAL_UINT8(3, s.item);
}

void test_flash_alternates_four_frames() {
  const FlashFrame expected[] = {FlashFrame::Regular, FlashFrame::Inverted, FlashFrame::Regular,
                                 FlashFrame::Inverted};
  TEST_ASSERT_EQUAL_UINT32(4, kFlashSteps);
  for (uint32_t i = 0; i < kFlashSteps; i++) {
    TEST_ASSERT_EQUAL(expected[i], flash_frame(i * kFlashStepMs));
    TEST_ASSERT_EQUAL(expected[i], flash_frame((i + 1) * kFlashStepMs - 1));
  }
  TEST_ASSERT_EQUAL(FlashFrame::Done, flash_frame(kFlashSteps * kFlashStepMs));
}

void test_alert_gate_limits_each_address() {
  AlertGate gate;
  const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t b[6] = {1, 2, 3, 4, 5, 7};
  TEST_ASSERT_TRUE(gate.admit(a, 1000));
  TEST_ASSERT_FALSE(gate.admit(a, 1000 + kAlertRepeatMs - 1));
  TEST_ASSERT_TRUE(gate.admit(b, 2000));  // another device still alerts
  TEST_ASSERT_TRUE(gate.admit(a, 1000 + kAlertRepeatMs));

  // A ninth address pushes out the one that alerted longest ago.
  AlertGate full;
  uint8_t mac[6] = {9, 9, 9, 9, 9, 0};
  for (uint8_t i = 0; i < 9; i++) {
    mac[5] = i;
    TEST_ASSERT_TRUE(full.admit(mac, 100 + i));
  }
  mac[5] = 0;
  TEST_ASSERT_TRUE(full.admit(mac, 200));
  mac[5] = 8;
  TEST_ASSERT_FALSE(full.admit(mac, 200));
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
  RUN_TEST(test_early_power_off_hold_only_explains);
  RUN_TEST(test_device_menu_selects_modes);
  RUN_TEST(test_start_dashboard_offers_usb_and_wifi);
  RUN_TEST(test_running_dashboard_is_stopped_directly);
  RUN_TEST(test_alerts_menu_toggles_in_place_and_returns);
  RUN_TEST(test_flash_alternates_four_frames);
  RUN_TEST(test_alert_gate_limits_each_address);
  return UNITY_END();
}
