// SPDX-License-Identifier: MIT
//
// Host unit tests for the NMEA parser: pio test -e native

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "fy_nmea.h"

using fy::gps::NmeaParser;

namespace {

// Wraps a sentence body (without '$' and '*') with a correct checksum.
std::string sentence(const char *body) {
  unsigned sum = 0;
  for (const char *p = body; *p; p++) sum ^= static_cast<unsigned char>(*p);
  char tail[8];
  snprintf(tail, sizeof(tail), "*%02X", sum);
  return std::string("$") + body + tail;
}

void feed_all(NmeaParser &p, const std::string &s) {
  for (char c : s) p.feed(c);
}

}  // namespace

// Reference sentences with known-good checksums (1994 date).
void test_reference_rmc_position_and_old_date() {
  NmeaParser p;
  const uint8_t u =
      p.parse_sentence("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
  TEST_ASSERT_TRUE(u & fy::gps::kUpdatedPosition);
  const auto &d = p.data();
  TEST_ASSERT_TRUE(d.position_valid);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.1173, d.latitude_deg);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 11.516667, d.longitude_deg);
  TEST_ASSERT_TRUE(d.speed_valid);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 22.4f, d.speed_knots);
  // A 1994 date is below the validity floor, as receiver placeholders are.
  TEST_ASSERT_FALSE(d.time_valid);
}

void test_reference_gga() {
  NmeaParser p;
  p.parse_sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
  const auto &d = p.data();
  TEST_ASSERT_TRUE(d.position_valid);
  TEST_ASSERT_EQUAL_UINT8(1, d.fix_quality);
  TEST_ASSERT_EQUAL_UINT8(8, d.satellites);
  TEST_ASSERT_TRUE(d.hdop_valid);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.9f, d.hdop);
  TEST_ASSERT_TRUE(d.altitude_valid);
  TEST_ASSERT_FLOAT_WITHIN(1e-2f, 545.4f, d.altitude_m);
}

void test_rmc_current_date_sets_time() {
  NmeaParser p;
  p.parse_sentence(
      sentence("GNRMC,174512.250,A,3746.4940,N,12225.1640,W,0.12,,201026,,,A").c_str());
  const auto &d = p.data();
  TEST_ASSERT_TRUE(d.time_valid);
  TEST_ASSERT_EQUAL_UINT16(2026, d.utc.year);
  TEST_ASSERT_EQUAL_UINT8(10, d.utc.month);
  TEST_ASSERT_EQUAL_UINT8(20, d.utc.day);
  TEST_ASSERT_EQUAL_UINT8(17, d.utc.hour);
  TEST_ASSERT_EQUAL_UINT8(45, d.utc.minute);
  TEST_ASSERT_EQUAL_UINT8(12, d.utc.second);
  TEST_ASSERT_EQUAL_UINT16(250, d.utc.millisecond);
  TEST_ASSERT_TRUE(d.position_valid);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 37.774900, d.latitude_deg);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, -122.419400, d.longitude_deg);
  TEST_ASSERT_FALSE(d.course_valid);  // empty field
}

void test_time_without_position() {
  NmeaParser p;
  p.parse_sentence(sentence("GNRMC,080000.000,V,,,,,,,201026,,,N").c_str());
  TEST_ASSERT_TRUE(p.data().time_valid);
  TEST_ASSERT_FALSE(p.data().position_valid);
}

void test_empty_rmc_before_any_fix() {
  NmeaParser p;
  p.parse_sentence(sentence("GNRMC,,V,,,,,,,,,,N").c_str());
  TEST_ASSERT_FALSE(p.data().time_valid);
  TEST_ASSERT_FALSE(p.data().position_valid);
  TEST_ASSERT_EQUAL_UINT32(1, p.sentences());
}

void test_fix_lost_clears_position() {
  NmeaParser p;
  p.parse_sentence(sentence("GNRMC,080000,A,3746.4940,N,12225.1640,W,0,0,201026,,,A").c_str());
  TEST_ASSERT_TRUE(p.data().position_valid);
  p.parse_sentence(sentence("GNRMC,080001,V,3746.4940,N,12225.1640,W,0,0,201026,,,N").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
}

void test_estimated_positions_are_invalid() {
  NmeaParser p;
  p.parse_sentence(sentence("GNRMC,080000,A,3746.4940,N,12225.1640,W,0,0,201026,,,E").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
  p.parse_sentence(sentence("GNGGA,080000,3746.4940,N,12225.1640,W,6,04,2.0,10,M,,M,,").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
  p.parse_sentence(sentence("GNGGA,080000,,,,,0,00,99.99,,,,,,").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
  TEST_ASSERT_EQUAL_UINT8(0, p.data().fix_quality);
}

void test_southern_and_western_hemispheres() {
  NmeaParser p;
  p.parse_sentence(sentence("GPGGA,000000,3352.1280,S,15112.5960,E,1,07,1.1,30,M,,M,,").c_str());
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, -33.868800, p.data().latitude_deg);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 151.209933, p.data().longitude_deg);
}

void test_gsa_fix_type() {
  NmeaParser p;
  TEST_ASSERT_EQUAL_UINT8(0, p.data().fix_type);
  const uint8_t u = p.parse_sentence(sentence("GNGSA,A,3,05,13,15,,,,,,,,,,1.8,1.0,1.5,1").c_str());
  TEST_ASSERT_EQUAL_UINT8(fy::gps::kUpdatedQuality, u);
  TEST_ASSERT_EQUAL_UINT8(3, p.data().fix_type);
}

void test_bad_checksum_is_rejected() {
  NmeaParser p;
  std::string s = sentence("GNRMC,080000,A,3746.4940,N,12225.1640,W,0,0,201026,,,A");
  s[s.size() - 1] = s[s.size() - 1] == '0' ? '1' : '0';
  TEST_ASSERT_EQUAL_UINT8(0, p.parse_sentence(s.c_str()));
  TEST_ASSERT_FALSE(p.data().position_valid);
  TEST_ASSERT_EQUAL_UINT32(1, p.checksum_errors());
  TEST_ASSERT_EQUAL_UINT8(0, p.parse_sentence("$GNRMC,080000,A"));  // no checksum at all
  TEST_ASSERT_EQUAL_UINT32(2, p.checksum_errors());
}

void test_malformed_coordinates_are_invalid() {
  NmeaParser p;
  p.parse_sentence(sentence("GNRMC,080000,A,3760.0000,N,12225.1640,W,0,0,201026,,,A").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);  // 60 minutes
  p.parse_sentence(sentence("GNRMC,080000,A,37x6.4940,N,12225.1640,W,0,0,201026,,,A").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
  p.parse_sentence(sentence("GNRMC,080000,A,3746.4940,Q,12225.1640,W,0,0,201026,,,A").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
  p.parse_sentence(sentence("GNRMC,080000,A,9100.0000,N,12225.1640,W,0,0,201026,,,A").c_str());
  TEST_ASSERT_FALSE(p.data().position_valid);
}

void test_malformed_time_and_date() {
  NmeaParser p;
  p.parse_sentence(sentence("GNRMC,256000,V,,,,,,,201026,,,N").c_str());
  TEST_ASSERT_FALSE(p.data().time_valid);  // hour 25
  p.parse_sentence(sentence("GNRMC,080000,V,,,,,,,201326,,,N").c_str());
  TEST_ASSERT_FALSE(p.data().time_valid);  // month 13
}

void test_stream_with_noise_and_overflow() {
  NmeaParser p;
  std::string stream = "garbage\r\n";
  stream += "$" + std::string(150, 'X') + "\r\n";  // too long: dropped
  stream += sentence("GPTXT,01,01,02,ANTSTATUS=OK") + "\r\n";
  stream += sentence("PCAS,03,1") + "\r\n";  // proprietary
  stream += sentence("GNGGA,080000,3746.4940,N,12225.1640,W,1,09,1.0,10,M,,M,,") + "\r\n";
  stream += sentence("GNRMC,080000,A,3746.4940,N,12225.1640,W,0,0,201026,,,A") + "\r\n";
  feed_all(p, stream);
  TEST_ASSERT_EQUAL_UINT32(1, p.overflows());
  TEST_ASSERT_EQUAL_UINT32(0, p.checksum_errors());
  TEST_ASSERT_EQUAL_UINT32(3, p.sentences());  // TXT, GGA, RMC (PCAS skipped)
  TEST_ASSERT_TRUE(p.data().position_valid);
  TEST_ASSERT_TRUE(p.data().time_valid);
  TEST_ASSERT_EQUAL_UINT8(9, p.data().satellites);
}

void test_gsv_cycles_sum_constellations() {
  NmeaParser p;
  // GPS: 6 in view over two messages, 4 with signal (one empty C/N0, one 0).
  TEST_ASSERT_EQUAL_UINT8(0, p.parse_sentence(sentence(
      "GPGSV,2,1,06,01,40,083,41,02,17,308,,03,07,344,29,04,55,123,00,0").c_str()));
  TEST_ASSERT_EQUAL_UINT8(0, p.data().satellites_in_view);  // cycle not complete yet
  TEST_ASSERT_EQUAL_UINT8(fy::gps::kUpdatedDiagnostics, p.parse_sentence(sentence(
      "GPGSV,2,2,06,05,20,200,33,06,10,010,38,0").c_str()));
  TEST_ASSERT_EQUAL_UINT8(6, p.data().satellites_in_view);
  TEST_ASSERT_EQUAL_UINT8(4, p.data().satellites_with_signal);
  TEST_ASSERT_EQUAL_UINT8(41, p.data().max_cn0);
  // BeiDou adds to the totals.
  p.parse_sentence(sentence("BDGSV,1,1,02,11,50,100,45,12,30,250,,0").c_str());
  TEST_ASSERT_EQUAL_UINT8(8, p.data().satellites_in_view);
  TEST_ASSERT_EQUAL_UINT8(5, p.data().satellites_with_signal);
  TEST_ASSERT_EQUAL_UINT8(45, p.data().max_cn0);
  // A later empty GPS cycle replaces the GPS share only.
  p.parse_sentence("$GPGSV,1,1,00,0*65");
  TEST_ASSERT_EQUAL_UINT8(2, p.data().satellites_in_view);
  TEST_ASSERT_EQUAL_UINT8(1, p.data().satellites_with_signal);
}

void test_gsv_with_nothing_in_view() {
  NmeaParser p;
  p.parse_sentence("$GPGSV,1,1,00,0*65");
  p.parse_sentence("$BDGSV,1,1,00,0*74");
  TEST_ASSERT_EQUAL_UINT8(0, p.data().satellites_in_view);
  TEST_ASSERT_EQUAL_UINT8(0, p.data().satellites_with_signal);
  TEST_ASSERT_EQUAL_UINT8(0, p.data().max_cn0);
}

void test_antenna_status_text() {
  NmeaParser p;
  TEST_ASSERT_EQUAL(fy::gps::Antenna::Unknown, p.data().antenna);
  TEST_ASSERT_EQUAL_UINT8(fy::gps::kUpdatedDiagnostics,
                          p.parse_sentence("$GPTXT,01,01,01,ANTENNA OPEN*25"));
  TEST_ASSERT_EQUAL(fy::gps::Antenna::Open, p.data().antenna);
  p.parse_sentence(sentence("GPTXT,01,01,01,ANTENNA OK").c_str());
  TEST_ASSERT_EQUAL(fy::gps::Antenna::Ok, p.data().antenna);
  p.parse_sentence(sentence("GPTXT,01,01,02,ANTSTATUS=SHORT").c_str());
  TEST_ASSERT_EQUAL(fy::gps::Antenna::Short, p.data().antenna);
  // Unrelated text leaves the state alone.
  TEST_ASSERT_EQUAL_UINT8(0, p.parse_sentence(sentence("GPTXT,01,01,02,MA=CASIC").c_str()));
  TEST_ASSERT_EQUAL(fy::gps::Antenna::Short, p.data().antenna);
}

void test_receiver_identification_text() {
  NmeaParser p;
  TEST_ASSERT_EQUAL_STRING("", p.data().receiver_info);
  // The text field contains a comma; it is rejoined.
  TEST_ASSERT_EQUAL_UINT8(fy::gps::kUpdatedDiagnostics,
                          p.parse_sentence(sentence("GPTXT,01,01,02,SW=URANUS5,V5.3.0.0").c_str()));
  TEST_ASSERT_EQUAL_STRING("SW=URANUS5,V5.3.0.0", p.data().receiver_info);
  p.parse_sentence(sentence("GPTXT,01,01,02,IC=AT6558R-5N-32-1C580901,B5Z9P1-D2-120417").c_str());
  TEST_ASSERT_EQUAL_STRING("IC=AT6558R-5N-32-1C580901,B5Z9P1-D2-120417", p.data().receiver_info);
}

void test_feed_reports_update_bits() {
  NmeaParser p;
  uint8_t last = 0;
  for (char c : sentence("GNRMC,080000,A,3746.4940,N,12225.1640,W,0,0,201026,,,A") + "\n") {
    const uint8_t u = p.feed(c);
    if (u) last = u;
  }
  TEST_ASSERT_TRUE(last & fy::gps::kUpdatedTime);
  TEST_ASSERT_TRUE(last & fy::gps::kUpdatedPosition);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reference_rmc_position_and_old_date);
  RUN_TEST(test_reference_gga);
  RUN_TEST(test_rmc_current_date_sets_time);
  RUN_TEST(test_time_without_position);
  RUN_TEST(test_empty_rmc_before_any_fix);
  RUN_TEST(test_fix_lost_clears_position);
  RUN_TEST(test_estimated_positions_are_invalid);
  RUN_TEST(test_southern_and_western_hemispheres);
  RUN_TEST(test_gsa_fix_type);
  RUN_TEST(test_bad_checksum_is_rejected);
  RUN_TEST(test_malformed_coordinates_are_invalid);
  RUN_TEST(test_malformed_time_and_date);
  RUN_TEST(test_stream_with_noise_and_overflow);
  RUN_TEST(test_gsv_cycles_sum_constellations);
  RUN_TEST(test_gsv_with_nothing_in_view);
  RUN_TEST(test_antenna_status_text);
  RUN_TEST(test_receiver_identification_text);
  RUN_TEST(test_feed_reports_update_bits);
  return UNITY_END();
}
