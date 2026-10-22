// SPDX-License-Identifier: MIT
//
// Host unit tests for the Wi-Fi dashboard credentials and status page:
// pio test -e native

#include <unity.h>

#include <cstring>
#include <initializer_list>
#include <vector>

#include "fy_dashboard_text.h"

using namespace fy::dashboard;

void setUp() {}
void tearDown() {}

namespace {

// Replays a fixed byte sequence, then counts upwards.
struct Bytes {
  std::vector<uint8_t> seq;
  size_t pos = 0;
  uint8_t counter = 0;
};

uint8_t next_byte(void *ctx) {
  Bytes *b = static_cast<Bytes *>(ctx);
  if (b->pos < b->seq.size()) return b->seq[b->pos++];
  return b->counter++;
}

bool in_alphabet(char c) { return c != '\0' && strchr(kAlphabet, c) != nullptr; }

StatusPage sample_page(const PageRow *rows, int n) {
  StatusPage p = {};
  p.uptime_s = 3725;
  p.detection_paused = true;
  p.idle_minutes = 10;
  p.battery_available = true;
  p.battery_valid = true;
  p.battery_mv = 4050;
  p.battery_percent = 87;
  p.gps_available = true;
  p.gps_fix = true;
  p.fix_type = 3;
  p.satellites = 9;
  p.reported = 12;
  p.reported_specific = 2;
  p.rows = rows;
  p.row_count = n;
  p.storage_available = true;
  p.storage_percent = 4;
  p.records_used = 1234;
  p.records_capacity = 32768;
  p.passkey_temporary = true;
  return p;
}

}  // namespace

void test_alphabet_avoids_confusable_characters() {
  TEST_ASSERT_EQUAL_UINT(31, strlen(kAlphabet));
  for (const char c : {'0', '1', 'i', 'l', 'o'}) TEST_ASSERT_NULL(strchr(kAlphabet, c));
}

void test_symbols_are_uniform_by_rejection() {
  // 248..255 would bias the draw toward the first symbols; they are skipped.
  Bytes b{{255, 248, 0}};
  TEST_ASSERT_EQUAL_INT('a', random_symbol(next_byte, &b));
  TEST_ASSERT_EQUAL_UINT(3, b.pos);
  Bytes c{{30, 31, 247}};
  TEST_ASSERT_EQUAL_INT('9', random_symbol(next_byte, &c));
  TEST_ASSERT_EQUAL_INT('a', random_symbol(next_byte, &c));
  TEST_ASSERT_EQUAL_INT(kAlphabet[247 % 31], random_symbol(next_byte, &c));
}

void test_passkey_format() {
  Bytes b;
  char key[16];
  TEST_ASSERT_TRUE(make_passkey(next_byte, &b, key, sizeof(key)));
  TEST_ASSERT_EQUAL_UINT(14, strlen(key));
  for (int i = 0; i < 14; i++) {
    if (i == 4 || i == 9) {
      TEST_ASSERT_EQUAL_INT('-', key[i]);
    } else {
      TEST_ASSERT_TRUE(in_alphabet(key[i]));
    }
  }
  TEST_ASSERT_EQUAL_STRING("abcd-efgh-jkmn", key);  // counter bytes 0..11
  char small[14];
  TEST_ASSERT_FALSE(make_passkey(next_byte, &b, small, sizeof(small)));
}

void test_ssid_format_and_limits() {
  Bytes b;
  char ssid[33];
  TEST_ASSERT_TRUE(make_ssid("FY-", next_byte, &b, ssid, sizeof(ssid)));
  TEST_ASSERT_EQUAL_STRING("FY-ABCD", ssid);
  // 28-character prefix + 4 = 32, the SSID limit; one more does not fit.
  const char *p28 = "abcdefghijklmnopqrstuvwxyz01";
  TEST_ASSERT_TRUE(make_ssid(p28, next_byte, &b, ssid, sizeof(ssid)));
  TEST_ASSERT_EQUAL_UINT(32, strlen(ssid));
  TEST_ASSERT_FALSE(make_ssid("abcdefghijklmnopqrstuvwxyz012", next_byte, &b, ssid, sizeof(ssid)));
  char small[7];
  TEST_ASSERT_FALSE(make_ssid("FY-", next_byte, &b, small, sizeof(small)));
}

void test_status_page_content() {
  const PageRow rows[] = {{"Flock camera", false, 2, 15}, {"Flock OUI?", true, 6, 38}};
  static char html[4096];
  const size_t n = render_status_page(sample_page(rows, 2), html, sizeof(html));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_UINT(strlen(html), n);
  TEST_ASSERT_NOT_NULL(strstr(html, "<!doctype html>"));
  TEST_ASSERT_NOT_NULL(strstr(html, "Detection is paused"));
  TEST_ASSERT_NOT_NULL(strstr(html, "after 10 minutes"));
  TEST_ASSERT_NOT_NULL(strstr(html, "87% (4.05 V)"));
  TEST_ASSERT_NOT_NULL(strstr(html, "3D fix, 9 satellites"));
  TEST_ASSERT_NOT_NULL(strstr(html, "<td>Flock camera</td><td>2</td><td>15</td>"));
  TEST_ASSERT_NOT_NULL(strstr(html, "4% used (1234 of 32768 records)"));
  TEST_ASSERT_NOT_NULL(strstr(html, "Uptime: 1h 02m"));
  TEST_ASSERT_NOT_NULL(strstr(html, "still the generated one"));
  // Read-only and self-contained: no scripts, refresh, forms, or links out.
  TEST_ASSERT_NULL(strstr(html, "<script"));
  TEST_ASSERT_NULL(strstr(html, "http-equiv"));
  TEST_ASSERT_NULL(strstr(html, "<form"));
  TEST_ASSERT_NULL(strstr(html, "http://"));
}

void test_status_page_variants() {
  static char html[4096];
  StatusPage p = sample_page(nullptr, 0);
  p.battery_available = false;
  p.gps_fix = false;
  p.passkey_temporary = false;
  TEST_ASSERT_TRUE(render_status_page(p, html, sizeof(html)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(html, "not measured on this board"));
  TEST_ASSERT_NOT_NULL(strstr(html, "GPS: no fix"));
  TEST_ASSERT_NOT_NULL(strstr(html, "No detections yet"));
  TEST_ASSERT_NULL(strstr(html, "still the generated one"));
}

void test_status_page_over_usb_says_detection_runs() {
  static char html[4096];
  StatusPage p = sample_page(nullptr, 0);
  p.detection_paused = false;  // served over USB, no Wi-Fi session
  TEST_ASSERT_TRUE(render_status_page(p, html, sizeof(html)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(html, "Detection is running."));
  TEST_ASSERT_NULL(strstr(html, "Detection is paused"));
}

void test_status_page_names_a_shared_passkey() {
  static char html[4096];
  StatusPage p = sample_page(nullptr, 0);
  p.passkey_shared = true;
  TEST_ASSERT_TRUE(render_status_page(p, html, sizeof(html)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(html, "shared one built into this firmware"));
  TEST_ASSERT_NULL(strstr(html, "shown on the device"));
}

void test_status_page_escapes_names() {
  const PageRow rows[] = {{"A<b>&\"", false, 1, 1}};
  static char html[4096];
  render_status_page(sample_page(rows, 1), html, sizeof(html));
  TEST_ASSERT_NOT_NULL(strstr(html, "<td>A&lt;b&gt;&amp;&quot;</td>"));
}

void test_status_page_too_small() {
  char html[256];
  TEST_ASSERT_EQUAL_UINT(0, render_status_page(sample_page(nullptr, 0), html, sizeof(html)));
  TEST_ASSERT_EQUAL_STRING("", html);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_alphabet_avoids_confusable_characters);
  RUN_TEST(test_symbols_are_uniform_by_rejection);
  RUN_TEST(test_passkey_format);
  RUN_TEST(test_ssid_format_and_limits);
  RUN_TEST(test_status_page_content);
  RUN_TEST(test_status_page_variants);
  RUN_TEST(test_status_page_over_usb_says_detection_runs);
  RUN_TEST(test_status_page_names_a_shared_passkey);
  RUN_TEST(test_status_page_escapes_names);
  RUN_TEST(test_status_page_too_small);
  return UNITY_END();
}
