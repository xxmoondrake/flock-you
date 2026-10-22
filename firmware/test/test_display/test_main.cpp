// SPDX-License-Identifier: MIT
//
// Host unit tests for the canvas and status-screen layout: pio test -e native
//
// Set FY_RENDER_DIR to a directory to also write each sample screen as a PBM
// image for visual review.

#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fy_canvas.h"
#include "fy_ui_layout.h"

using fy::display::Canvas;
using fy::ui::UiModel;
using fy::ui::UiState;

namespace {

void maybe_write_pbm(const Canvas &c, const char *name) {
  const char *dir = getenv("FY_RENDER_DIR");
  if (!dir || !*dir) return;
  const std::string path = std::string(dir) + "/" + name + ".pbm";
  FILE *f = fopen(path.c_str(), "w");
  if (!f) return;
  fprintf(f, "P1\n%d %d\n", fy::display::kWidth, fy::display::kHeight);
  for (int y = 0; y < fy::display::kHeight; y++) {
    for (int x = 0; x < fy::display::kWidth; x++) fputs(c.pixel(x, y) ? "1 " : "0 ", f);
    fputc('\n', f);
  }
  fclose(f);
}

int lit_pixels(const Canvas &c, int x0, int y0, int w, int h) {
  int n = 0;
  for (int y = y0; y < y0 + h; y++) {
    for (int x = x0; x < x0 + w; x++) n += c.pixel(x, y) ? 1 : 0;
  }
  return n;
}

UiModel base_model() {
  UiModel m = {};
  m.now_ms = 754000;
  m.wifi_capture = true;
  m.ble_scan = true;
  m.channel = 11;
  m.gps_available = true;
  m.gps_receiving = true;
  m.sats_in_view = 0;
  m.sats_with_signal = 0;
  m.antenna = 2;  // open: a passive patch antenna can report this and still work
  return m;
}

}  // namespace

void test_pixels_use_ssd1306_page_order() {
  Canvas c;
  c.set_pixel(0, 0, true);
  c.set_pixel(5, 9, true);
  c.set_pixel(127, 63, true);
  TEST_ASSERT_EQUAL_HEX8(0x01, c.frame()[0]);
  TEST_ASSERT_EQUAL_HEX8(0x02, c.frame()[128 + 5]);
  TEST_ASSERT_EQUAL_HEX8(0x80, c.frame()[7 * 128 + 127]);
  c.set_pixel(5, 9, false);
  TEST_ASSERT_EQUAL_HEX8(0x00, c.frame()[128 + 5]);
}

void test_out_of_range_pixels_are_clipped() {
  Canvas c;
  c.set_pixel(-1, 0, true);
  c.set_pixel(128, 0, true);
  c.set_pixel(0, 64, true);
  c.fill_rect(120, 60, 20, 20, true);
  TEST_ASSERT_EQUAL_INT(8 * 4, lit_pixels(c, 0, 0, 128, 64));
}

void test_text_advance_and_unprintable_bytes() {
  Canvas a, b;
  TEST_ASSERT_EQUAL_INT(18, a.draw_text(0, 0, "abc"));
  a.draw_text(0, 10, "\x01");
  b.draw_text(0, 10, "?");
  for (int y = 10; y < 18; y++) {
    for (int x = 0; x < 6; x++) TEST_ASSERT_EQUAL(b.pixel(x, y), a.pixel(x, y));
  }
  TEST_ASSERT_TRUE(lit_pixels(a, 0, 10, 6, 8) > 0);
}

void test_every_printable_glyph_has_pixels_except_space() {
  for (int ch = 0x21; ch <= 0x7e; ch++) {
    Canvas c;
    const char s[2] = {static_cast<char>(ch), 0};
    c.draw_text(0, 0, s);
    TEST_ASSERT_TRUE_MESSAGE(lit_pixels(c, 0, 0, 6, 8) > 0, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lit_pixels(c, 5, 0, 1, 8), s);  // spacing column empty
  }
}

void test_format_age() {
  char b[8];
  fy::ui::format_age(12000, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("12s", b);
  fy::ui::format_age(240000, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("4m", b);
  fy::ui::format_age(3 * 3600000u, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("3h", b);
  fy::ui::format_age(2 * 86400000u, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("2d", b);
}

void test_screen_searching_shows_diagnostics() {
  Canvas c;
  fy::ui::render_home(c, base_model());
  maybe_write_pbm(c, "screen_searching");
  TEST_ASSERT_EQUAL_INT(128, lit_pixels(c, 0, 36, 128, 1));  // separator
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 27, 128, 8) > 0);        // diagnostics row
}

void test_screen_searching_with_signal() {
  UiModel m = base_model();
  m.sats_in_view = 11;
  m.sats_with_signal = 4;
  m.antenna = 1;
  m.battery_available = true;
  m.battery_valid = true;
  m.battery_mv = 4050;
  m.battery_percent = 81;
  Canvas c;
  fy::ui::render_home(c, m);
  maybe_write_pbm(c, "screen_searching_signal");
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 27, 128, 8) > 0);
}

void test_screen_fix_and_possible_match() {
  UiModel m = base_model();
  m.gps_time_valid = true;
  m.utc_hour = 17;
  m.utc_minute = 45;
  m.utc_second = 12;
  m.gps_time_age_ms = 400;
  m.gps_position_current = true;
  m.gps_position_ever = true;
  m.latitude_deg = 37.77490;
  m.longitude_deg = -122.41940;
  m.fix_type = 3;
  m.satellites = 9;
  m.hdop_valid = true;
  m.hdop = 1.2f;
  m.reported = 3;
  m.last_valid = true;
  m.last_rule = "wifi_oui_addr2";
  m.last_rssi = -81;
  m.last_at_ms = m.now_ms - 95000;
  Canvas c;
  fy::ui::render_home(c, m);
  maybe_write_pbm(c, "screen_fix_possible");
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 27, 128, 8) > 0);  // coordinates shown
}

void test_screen_specific_alert_is_inverted() {
  UiModel m = base_model();
  m.reported = 5;
  m.reported_specific = 1;
  m.last_valid = true;
  m.last_specific = true;
  m.last_rule = "ble_name_fs_ext_battery";
  m.last_rssi = -67;
  m.last_at_ms = m.now_ms - 2000;
  m.alert_active = true;
  m.alert_age_ms = 2000;
  m.gps_position_ever = true;
  m.gps_position_age_ms = 45000;
  Canvas c;
  fy::ui::render_home(c, m);
  maybe_write_pbm(c, "screen_specific_alert");
  TEST_ASSERT_TRUE(c.pixel(127, 3));  // banner background reaches the right edge
}

void test_battery_title_differs_from_plain_title() {
  UiModel plain = base_model();
  UiModel with_battery = base_model();
  with_battery.battery_available = true;
  with_battery.battery_valid = true;
  with_battery.battery_mv = 3712;
  with_battery.battery_percent = 15;
  UiModel no_reading = base_model();
  no_reading.battery_available = true;
  Canvas a, b, c;
  fy::ui::render_home(a, plain);
  fy::ui::render_home(b, with_battery);
  fy::ui::render_home(c, no_reading);
  maybe_write_pbm(b, "screen_battery");
  bool differs_ab = false, differs_ac = false;
  for (int x = 0; x < 64; x++) {
    for (int y = 0; y < 8; y++) {
      differs_ab |= a.pixel(x, y) != b.pixel(x, y);
      differs_ac |= a.pixel(x, y) != c.pixel(x, y);
    }
  }
  TEST_ASSERT_TRUE(differs_ab);
  TEST_ASSERT_TRUE(differs_ac);
}

void test_storage_warning_replaces_match_row() {
  UiModel m = base_model();
  m.storage_warning = true;
  m.storage_percent = 93;
  Canvas c;
  fy::ui::render_home(c, m);
  maybe_write_pbm(c, "screen_storage_warning");
  TEST_ASSERT_TRUE(c.pixel(127, 40));  // inverted row reaches the right edge
}

void test_screen_no_gps_data_and_paused_wifi() {
  UiModel m = base_model();
  m.wifi_capture = false;
  m.gps_receiving = false;
  Canvas c;
  fy::ui::render_home(c, m);
  maybe_write_pbm(c, "screen_paused_nodata");
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 9, 128, 8) > 0);
  TEST_ASSERT_EQUAL_INT(0, lit_pixels(c, 0, 27, 128, 8));  // no diagnostics without data
}

// ---- Detections, data management, menus ---------------------------------------

UiModel data_model() {
  UiModel m = base_model();
  m.storage_available = true;
  m.records_used = 1234;
  m.records_capacity = 32768;
  m.storage_percent = 3;
  m.encounter_records = 412;
  m.battery_records = 822;
  m.oldest_utc = 1767225600;  // 2026-01-01T00:00:00Z
  return m;
}

void test_detections_screen_lists_types() {
  UiModel m = base_model();
  m.detections[0] = {"Flock camera", false, 2, 15};
  m.detections[1] = {"Penguin bat", false, 1, 4};
  m.detections[2] = {"Flock OUI?", true, 6, 38};
  m.detection_rows = 3;
  Canvas c;
  fy::ui::render_detections(c, m);
  maybe_write_pbm(c, "screen_detections");
  TEST_ASSERT_TRUE(c.pixel(127, 3));                     // inverted title bar
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 11, 128, 8) > 0);    // first type row
  TEST_ASSERT_EQUAL_INT(0, lit_pixels(c, 0, 38, 128, 26));  // nothing after the list
}

void test_detections_screen_overflow_and_empty() {
  UiModel m = base_model();
  for (int i = 0; i < 8; i++) m.detections[i] = {"Type", false, 1, 1};
  m.detection_rows = 8;
  Canvas c;
  fy::ui::render_detections(c, m);
  maybe_write_pbm(c, "screen_detections_overflow");
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 56, 128, 8) > 0);  // "+3 more types"

  UiModel empty = base_model();
  Canvas e;
  fy::ui::render_detections(e, empty);
  TEST_ASSERT_TRUE(lit_pixels(e, 0, 20, 128, 8) > 0);  // "No detections yet"
}

void test_data_screen() {
  Canvas c;
  fy::ui::render_data(c, data_model());
  maybe_write_pbm(c, "screen_data");
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 56, 128, 8) > 0);  // menu hint
}

void test_menu_and_confirm_render_through_dispatch() {
  UiModel m = data_model();
  UiState s;
  s.screen = fy::ui::Screen::Data;
  s.overlay = fy::ui::Overlay::DataMenu;
  s.item = 1;
  Canvas menu;
  fy::ui::render(menu, m, s);
  maybe_write_pbm(menu, "screen_data_menu");
  TEST_ASSERT_TRUE(menu.pixel(127, 24));  // highlighted second item

  s.overlay = fy::ui::Overlay::EraseConfirm;
  s.item = 0;
  Canvas confirm;
  fy::ui::render(confirm, m, s);
  maybe_write_pbm(confirm, "screen_erase_confirm");
  TEST_ASSERT_TRUE(confirm.pixel(127, 32));  // "Cancel" highlighted
}

void test_erase_progress_and_power_off_prompts() {
  UiModel m = data_model();
  UiState s;
  m.erasing = true;
  m.erase_percent = 40;
  Canvas erasing;
  fy::ui::render(erasing, m, s);
  maybe_write_pbm(erasing, "screen_erasing");
  TEST_ASSERT_TRUE(erasing.pixel(40, 30));    // filled part of the bar
  TEST_ASSERT_FALSE(erasing.pixel(100, 30));  // unfilled part

  m.erasing = false;
  m.power_off_armed = true;
  Canvas off;
  fy::ui::render(off, m, s);
  maybe_write_pbm(off, "screen_power_off");
  TEST_ASSERT_TRUE(off.pixel(127, 22));
}

void test_low_battery_notice() {
  Canvas running, starting;
  fy::ui::render_low_battery(running, 3184, false);
  fy::ui::render_low_battery(starting, 3301, true);
  maybe_write_pbm(running, "screen_low_battery");
  maybe_write_pbm(starting, "screen_low_battery_start");
  TEST_ASSERT_TRUE(running.pixel(127, 14));  // inverted banner
  bool differs = false;
  for (int y = 40; y < 48; y++) {
    for (int x = 0; x < 128; x++) differs |= running.pixel(x, y) != starting.pixel(x, y);
  }
  TEST_ASSERT_TRUE(differs);  // different reason line
}

// ---- Device management ---------------------------------------------------------

void test_home_flags_standby_and_dashboard() {
  Canvas scanning, standby, dashboard;
  UiModel m = base_model();
  fy::ui::render_home(scanning, m);
  TEST_ASSERT_FALSE(scanning.pixel(127, 12));  // capture row not inverted

  m.standby = true;
  m.wifi_capture = false;
  m.ble_scan = false;
  fy::ui::render_home(standby, m);
  maybe_write_pbm(standby, "screen_home_standby");
  TEST_ASSERT_TRUE(standby.pixel(127, 12));  // inverted "STANDBY: NOT SCANNING"

  m.standby = false;
  m.dashboard_active = true;
  fy::ui::render_home(dashboard, m);
  maybe_write_pbm(dashboard, "screen_home_dashboard");
  TEST_ASSERT_TRUE(dashboard.pixel(127, 12));
  bool differs = false;
  for (int x = 0; x < 128; x++) differs |= standby.pixel(x, 12) != dashboard.pixel(x, 12);
  TEST_ASSERT_TRUE(differs);
}

void test_device_screen_without_dashboard() {
  UiModel m = base_model();
  Canvas c;
  fy::ui::render_device(c, m);
  maybe_write_pbm(c, "screen_device");
  TEST_ASSERT_TRUE(c.pixel(127, 3));                    // inverted title bar
  TEST_ASSERT_FALSE(c.pixel(127, 23));                  // dashboard row not inverted
  TEST_ASSERT_EQUAL_INT(0, lit_pixels(c, 0, 38, 128, 8));  // no failure notice
  TEST_ASSERT_TRUE(lit_pixels(c, 0, 56, 128, 8) > 0);   // menu hint

  m.dashboard_failed = true;
  Canvas failed;
  fy::ui::render_device(failed, m);
  TEST_ASSERT_TRUE(lit_pixels(failed, 0, 38, 128, 8) > 0);
}

void test_device_screen_shows_connection_details() {
  UiModel m = base_model();
  m.dashboard_active = true;
  m.dashboard_left_ms = 540000;
  snprintf(m.dashboard_ssid, sizeof(m.dashboard_ssid), "FY-K7QM");
  snprintf(m.dashboard_passkey, sizeof(m.dashboard_passkey), "k7qm-3xtd-ap9w");
  snprintf(m.dashboard_address, sizeof(m.dashboard_address), "192.168.4.1");
  m.dashboard_clients = 1;
  Canvas with_key;
  fy::ui::render_device(with_key, m);
  maybe_write_pbm(with_key, "screen_device_dashboard");
  TEST_ASSERT_TRUE(with_key.pixel(127, 23));                 // inverted time-left row
  TEST_ASSERT_TRUE(lit_pixels(with_key, 0, 29, 128, 8) > 0);  // SSID
  TEST_ASSERT_TRUE(lit_pixels(with_key, 0, 38, 128, 8) > 0);  // passkey
  TEST_ASSERT_TRUE(lit_pixels(with_key, 0, 47, 128, 8) > 0);  // address

  // Once the passkey has been changed it is never shown.
  m.dashboard_passkey[0] = '\0';
  Canvas changed;
  fy::ui::render_device(changed, m);
  TEST_ASSERT_EQUAL_INT(0, lit_pixels(changed, 0, 38, 128, 8));
}

void test_device_menu_renders_through_dispatch() {
  UiModel m = base_model();
  UiState s;
  s.screen = fy::ui::Screen::Device;
  s.overlay = fy::ui::Overlay::DeviceMenu;
  s.item = 2;
  Canvas menu;
  fy::ui::render(menu, m, s);
  maybe_write_pbm(menu, "screen_device_menu");
  TEST_ASSERT_TRUE(menu.pixel(127, 34));   // highlighted dashboard item
  TEST_ASSERT_FALSE(menu.pixel(127, 24));  // standby item not highlighted

  // The dashboard item's label follows its state.
  m.dashboard_active = true;
  Canvas stop;
  fy::ui::render(stop, m, s);
  bool differs = false;
  for (int y = 33; y < 41; y++) {
    for (int x = 0; x < 128; x++) differs |= menu.pixel(x, y) != stop.pixel(x, y);
  }
  TEST_ASSERT_TRUE(differs);
}

void test_usb_dashboard_screens() {
  UiModel m = base_model();
  m.dashboard_active = true;
  m.dashboard_usb = true;
  snprintf(m.dashboard_address, sizeof(m.dashboard_address), "192.168.7.1");
  Canvas home;
  fy::ui::render_home(home, m);
  TEST_ASSERT_FALSE(home.pixel(127, 12));  // detection keeps running: no inverted row
  Canvas device;
  fy::ui::render_device(device, m);
  maybe_write_pbm(device, "screen_device_usb");
  TEST_ASSERT_TRUE(lit_pixels(device, 0, 20, 128, 8) > 0);  // still detecting
  TEST_ASSERT_TRUE(device.pixel(127, 32));                   // inverted "USB dashboard on"
  TEST_ASSERT_TRUE(lit_pixels(device, 0, 38, 128, 8) > 0);  // page address
}

void test_dashboard_submenu_renders() {
  UiModel m = base_model();
  UiState s;
  s.screen = fy::ui::Screen::Device;
  s.overlay = fy::ui::Overlay::DashboardMenu;
  s.item = 1;
  Canvas c;
  fy::ui::render(c, m, s);
  maybe_write_pbm(c, "screen_dashboard_menu");
  TEST_ASSERT_TRUE(c.pixel(127, 3));    // title bar
  TEST_ASSERT_TRUE(c.pixel(127, 24));   // highlighted "WiFi dashboard"
  TEST_ASSERT_FALSE(c.pixel(127, 14));  // "USB dashboard" not highlighted
}

void test_message_uses_bottom_row() {
  UiModel m = base_model();
  UiState s;
  fy::ui::show_message(s, "Records erased", m.now_ms);
  Canvas c;
  fy::ui::render(c, m, s);
  maybe_write_pbm(c, "screen_message");
  TEST_ASSERT_TRUE(c.pixel(127, 58));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_detections_screen_lists_types);
  RUN_TEST(test_detections_screen_overflow_and_empty);
  RUN_TEST(test_data_screen);
  RUN_TEST(test_menu_and_confirm_render_through_dispatch);
  RUN_TEST(test_erase_progress_and_power_off_prompts);
  RUN_TEST(test_message_uses_bottom_row);
  RUN_TEST(test_home_flags_standby_and_dashboard);
  RUN_TEST(test_device_screen_without_dashboard);
  RUN_TEST(test_device_screen_shows_connection_details);
  RUN_TEST(test_device_menu_renders_through_dispatch);
  RUN_TEST(test_usb_dashboard_screens);
  RUN_TEST(test_dashboard_submenu_renders);
  RUN_TEST(test_low_battery_notice);
  RUN_TEST(test_pixels_use_ssd1306_page_order);
  RUN_TEST(test_out_of_range_pixels_are_clipped);
  RUN_TEST(test_text_advance_and_unprintable_bytes);
  RUN_TEST(test_every_printable_glyph_has_pixels_except_space);
  RUN_TEST(test_format_age);
  RUN_TEST(test_screen_searching_shows_diagnostics);
  RUN_TEST(test_screen_searching_with_signal);
  RUN_TEST(test_screen_fix_and_possible_match);
  RUN_TEST(test_screen_specific_alert_is_inverted);
  RUN_TEST(test_battery_title_differs_from_plain_title);
  RUN_TEST(test_storage_warning_replaces_match_row);
  RUN_TEST(test_screen_no_gps_data_and_paused_wifi);
  return UNITY_END();
}
