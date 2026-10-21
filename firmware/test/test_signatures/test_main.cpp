// SPDX-License-Identifier: MIT
//
// Host unit tests for fy_signatures: pio test -e native

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "fy_dashboard_json.h"
#include "fy_signatures.h"

using fy::Match;
using fy::Rule;
using fy::Strength;

namespace {

using Bytes = std::vector<uint8_t>;

const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t kFlockMac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, 0x03};      // target OUI
const uint8_t kOtherMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};      // not a target
const uint8_t kQualcommMac[6] = {0x00, 0x03, 0x7f, 0x50, 0x00, 0x01};   // target OUI

void append(Bytes &b, std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes); }

void append_ie(Bytes &b, uint8_t id, std::initializer_list<uint8_t> payload) {
  b.push_back(id);
  b.push_back(static_cast<uint8_t>(payload.size()));
  b.insert(b.end(), payload);
}

// 24-byte 802.11 header. fc0 0x40 = management probe request, 0x08 = data.
Bytes header(uint8_t fc0, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3) {
  Bytes b = {fc0, 0x00, 0x00, 0x00};
  b.insert(b.end(), a1, a1 + 6);
  b.insert(b.end(), a2, a2 + 6);
  b.insert(b.end(), a3, a3 + 6);
  append(b, {0x00, 0x00});
  return b;
}

// IEs that produce the drive-tested primary signature
// "2,12,127,221:506f9a16030103,45,191,221:0050f208000000".
void append_primary_ies(Bytes &b) {
  append_ie(b, 0, {});                                  // wildcard SSID (skipped)
  append_ie(b, 2, {0x01});
  append_ie(b, 12, {0x00, 0x00});
  append_ie(b, 127, {0x00, 0x00, 0x08, 0x00});
  append_ie(b, 221, {0x50, 0x6f, 0x9a, 0x16, 0x03, 0x01, 0x03});
  append_ie(b, 45, {0x00, 0x00});
  append_ie(b, 191, {0x00, 0x00});
  append_ie(b, 221, {0x00, 0x50, 0xf2, 0x08, 0x00, 0x00, 0x00});
}

Bytes primary_probe(const uint8_t *transmitter) {
  Bytes f = header(0x40, kBroadcast, transmitter, kBroadcast);
  append_primary_ies(f);
  return f;
}

size_t match_frame(const Bytes &f, bool is_mgmt, Match *out) {
  return fy::match_wifi_frame(f.data(), f.size(), is_mgmt, out, fy::kMaxWifiMatchesPerFrame);
}

Bytes ad_name(const char *name, uint8_t type = 0x09) {
  Bytes b;
  b.push_back(static_cast<uint8_t>(strlen(name) + 1));
  b.push_back(type);
  b.insert(b.end(), name, name + strlen(name));
  return b;
}

bool match_ad(const Bytes &ad, Match *out) {
  return fy::match_ble_advert(kOtherMac, ad.data(), ad.size(), out);
}

}  // namespace

// ---- Wi-Fi -----------------------------------------------------------------

void test_oui_table_size_matches_upstream() { TEST_ASSERT_EQUAL_UINT(34, fy::target_oui_count()); }

void test_primary_signature_is_built() {
  Bytes body;
  append_primary_ies(body);
  char sig[128];
  TEST_ASSERT_TRUE(fy::build_probe_ie_signature(body.data(), body.size(), sig, sizeof(sig)));
  TEST_ASSERT_EQUAL_STRING("2,12,127,221:506f9a16030103,45,191,221:0050f208000000", sig);
}

void test_probe_with_primary_signature_is_specific() {
  Match m[3];
  const Bytes f = primary_probe(kFlockMac);
  TEST_ASSERT_TRUE(fy::wifi_frame_is_candidate(f.data(), f.size(), true));
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiProbeIeSignature, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Specific, fy::rule_info(m[0].rule).strength);
  TEST_ASSERT_EQUAL_MEMORY(kFlockMac, m[0].mac, 6);
}

void test_primary_signature_survives_trailing_fcs() {
  Match m[3];
  Bytes f = primary_probe(kFlockMac);
  append(f, {0xde, 0xad, 0xbe, 0xef});
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiProbeIeSignature, m[0].rule);
}

void test_wildcard_probe_without_signature_is_possible() {
  Match m[3];
  Bytes f = header(0x40, kBroadcast, kFlockMac, kBroadcast);
  append_ie(f, 0, {});
  append_ie(f, 1, {0x82, 0x84});
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiWildcardProbe, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Possible, fy::rule_info(m[0].rule).strength);
}

void test_directed_probe_is_transmitter_oui_match() {
  Match m[3];
  Bytes f = header(0x40, kBroadcast, kFlockMac, kBroadcast);
  append_ie(f, 0, {'h', 'o', 'm', 'e'});
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiTransmitter, m[0].rule);
}

void test_receiver_oui_on_data_frame() {
  Match m[3];
  const Bytes f = header(0x08, kQualcommMac, kOtherMac, kQualcommMac);
  // addr3 is only checked on management frames.
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, false, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiReceiver, m[0].rule);
  TEST_ASSERT_EQUAL_MEMORY(kQualcommMac, m[0].mac, 6);
}

void test_bssid_oui_on_management_frame() {
  Match m[3];
  const Bytes f = header(0x80, kBroadcast, kOtherMac, kFlockMac);  // beacon
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiBssid, m[0].rule);
}

void test_unrelated_frame_has_no_match() {
  Match m[3];
  Bytes f = header(0x40, kBroadcast, kOtherMac, kBroadcast);
  append_primary_ies(f);
  TEST_ASSERT_FALSE(fy::wifi_frame_is_candidate(f.data(), f.size(), true));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(f, true, m));
}

void test_short_frame_is_rejected() {
  Match m[3];
  const Bytes f = primary_probe(kFlockMac);
  TEST_ASSERT_FALSE(fy::wifi_frame_is_candidate(f.data(), 23, true));
  TEST_ASSERT_EQUAL_UINT(0, fy::match_wifi_frame(f.data(), 23, true, m, 3));
}

void test_corrupt_ies_do_not_crash() {
  Match m[3];
  Bytes f = header(0x40, kBroadcast, kFlockMac, kBroadcast);
  append(f, {0x00, 0x00, 0x2d, 0xff, 0x01});  // IE length overruns the frame
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiWildcardProbe, m[0].rule);
}

void test_output_capacity_is_respected() {
  Match m[1];
  const Bytes f = header(0x80, kQualcommMac, kFlockMac, kFlockMac);
  TEST_ASSERT_EQUAL_UINT(1, fy::match_wifi_frame(f.data(), f.size(), true, m, 1));
  TEST_ASSERT_EQUAL(Rule::WifiOuiTransmitter, m[0].rule);
}

// ---- BLE -------------------------------------------------------------------

void test_penguin_serial_name_is_specific() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("Penguin-0123456789"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNamePenguinSerial, m.rule);
  TEST_ASSERT_EQUAL(Strength::Specific, fy::rule_info(m.rule).strength);
  TEST_ASSERT_EQUAL_STRING("Penguin-0123456789", m.name);
  TEST_ASSERT_EQUAL_MEMORY(kOtherMac, m.mac, 6);
}

void test_penguin_name_needs_ten_digits() {
  Match m;
  TEST_ASSERT_FALSE(match_ad(ad_name("Penguin-12345"), &m));
  TEST_ASSERT_FALSE(match_ad(ad_name("Penguin-0123456789x"), &m));
}

void test_battery_name_is_case_insensitive() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("fs EXT battery", 0x08), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameExtBattery, m.rule);
}

void test_generic_names_are_possible() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("0123456789"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameBareSerial, m.rule);
  TEST_ASSERT_TRUE(match_ad(ad_name("DfuTarg"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameDfuTarg, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, fy::rule_info(m.rule).strength);
}

void test_company_id_is_possible() {
  Match m;
  const Bytes ad = {0x05, 0xff, 0xc8, 0x09, 0x01, 0x02};
  TEST_ASSERT_TRUE(match_ad(ad, &m));
  TEST_ASSERT_EQUAL(Rule::BleMfgXuntong, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, fy::rule_info(m.rule).strength);
}

void test_flock_service_uuid_is_specific() {
  Match m;
  // e8ccbb38-9532-46a8-9fe5-1814df172e6f, little-endian on air.
  const Bytes ad = {0x11, 0x07, 0x6f, 0x2e, 0x17, 0xdf, 0x14, 0x18, 0xe5, 0x9f,
                    0xa8, 0x46, 0x32, 0x95, 0x38, 0xbb, 0xcc, 0xe8};
  TEST_ASSERT_TRUE(match_ad(ad, &m));
  TEST_ASSERT_EQUAL(Rule::BleServiceFlock, m.rule);
}

void test_nordic_dfu_service_is_possible() {
  Match m;
  // 00001530-1212-efde-1523-785feabcd123, little-endian on air.
  const Bytes ad = {0x11, 0x06, 0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
                    0xde, 0xef, 0x12, 0x12, 0x30, 0x15, 0x00, 0x00};
  TEST_ASSERT_TRUE(match_ad(ad, &m));
  TEST_ASSERT_EQUAL(Rule::BleServiceNordicDfu, m.rule);
}

void test_raven_service_range() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(Bytes{0x05, 0x03, 0x0f, 0x18, 0x00, 0x31}, &m));  // 0x180f, 0x3100
  TEST_ASSERT_EQUAL(Rule::BleServiceRaven, m.rule);
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0x02, 0x00, 0x35}, &m));  // 0x3500, inclusive
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0x03, 0x01, 0x35}, &m));  // 0x3501
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0x03, 0xff, 0x30}, &m));  // 0x30ff
}

void test_specific_rule_wins_over_possible() {
  Match m;
  Bytes ad = ad_name("DfuTarg");
  append(ad, {0x05, 0x03, 0x00, 0x32, 0x0f, 0x18});  // Raven 0x3200
  TEST_ASSERT_TRUE(match_ad(ad, &m));
  TEST_ASSERT_EQUAL(Rule::BleServiceRaven, m.rule);
  TEST_ASSERT_EQUAL_STRING("DfuTarg", m.name);
}

void test_malformed_ad_is_safe() {
  Match m;
  TEST_ASSERT_FALSE(match_ad(Bytes{0x1f, 0x09, 'P', 'e'}, &m));  // length overruns
  TEST_ASSERT_FALSE(match_ad(Bytes{0x00, 0x09, 0x41}, &m));      // zero-length field
  TEST_ASSERT_FALSE(match_ad(Bytes{0x01}, &m));                  // truncated header
  TEST_ASSERT_FALSE(fy::match_ble_advert(kOtherMac, nullptr, 0, &m));
}

void test_unrelated_advert_has_no_match() {
  Match m;
  Bytes ad = {0x02, 0x01, 0x06};
  const Bytes name = ad_name("Android");
  ad.insert(ad.end(), name.begin(), name.end());
  TEST_ASSERT_FALSE(match_ad(ad, &m));
}

void test_every_rule_has_a_device_type() {
  for (int r = 0; r < static_cast<int>(Rule::Count); r++) {
    const fy::DeviceType t = fy::device_type(static_cast<Rule>(r));
    TEST_ASSERT_TRUE(static_cast<int>(t) < static_cast<int>(fy::DeviceType::Count));
    const fy::DeviceTypeInfo &info = fy::device_type_info(t);
    TEST_ASSERT_TRUE(strlen(info.name) <= 12);
    // A specific rule never lands in a possible-only type.
    if (fy::rule_info(static_cast<Rule>(r)).strength == Strength::Specific) {
      TEST_ASSERT_FALSE(info.possible);
    }
  }
  TEST_ASSERT_EQUAL(fy::DeviceType::FlockCamera, fy::device_type(Rule::WifiProbeIeSignature));
  TEST_ASSERT_EQUAL(fy::DeviceType::FlockRaven, fy::device_type(Rule::BleServiceRaven));
  TEST_ASSERT_TRUE(fy::device_type_info(fy::device_type(Rule::WifiOuiReceiver)).possible);
}

// ---- Upstream dashboard JSON ------------------------------------------------

Match make_match(Rule rule, const uint8_t *mac, const char *name) {
  Match m = {};
  m.rule = rule;
  memcpy(m.mac, mac, sizeof(m.mac));
  snprintf(m.name, sizeof(m.name), "%s", name);
  return m;
}

void test_dashboard_methods_follow_upstream() {
  for (int r = 0; r < static_cast<int>(Rule::Count); r++) {
    const fy::RuleInfo &info = fy::rule_info(static_cast<Rule>(r));
    TEST_ASSERT_NOT_NULL(info.upstream_method);
    const char *prefix = info.transport == fy::Transport::Ble ? "ble_" : "wifi_";
    TEST_ASSERT_EQUAL_INT(0, strncmp(info.upstream_method, prefix, strlen(prefix)));
    TEST_ASSERT_TRUE(info.upstream_tier <= 4);
  }
  TEST_ASSERT_EQUAL_STRING("wifi_wildcard_probe_ie_sig",
                           fy::rule_info(Rule::WifiProbeIeSignature).upstream_method);
  TEST_ASSERT_EQUAL_STRING("ble_gatt_svc", fy::rule_info(Rule::BleServiceRaven).upstream_method);
}

void test_dashboard_json_wifi_fingerprint() {
  const Match m = make_match(Rule::WifiProbeIeSignature, kFlockMac, "");
  char line[400];
  const size_t n = fy::format_dashboard_detection(m, -52, 6, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING(
      "{\"event\":\"detection\",\"detection_method\":\"wifi_wildcard_probe_ie_sig\","
      "\"detection_tier\":4,\"protocol\":\"wifi_2_4ghz\",\"mac_address\":\"b4:1e:52:01:02:03\","
      "\"oui\":\"b4:1e:52\",\"device_name\":\"\",\"rssi\":-52,\"channel\":6,\"frequency\":2437,"
      "\"ssid\":\"\",\"rule\":\"wifi_probe_ie_sig\",\"strength\":\"specific\"}",
      line);
  TEST_ASSERT_EQUAL_UINT(strlen(line), n);
}

void test_dashboard_json_ble_name() {
  const uint8_t mac[6] = {0xd4, 0x01, 0x02, 0x03, 0x04, 0x05};
  const Match m = make_match(Rule::BleNamePenguinSerial, mac, "Penguin-1234567890");
  char line[400];
  fy::format_dashboard_detection(m, -71, 11, line, sizeof(line));
  // BLE reports channel 255 and frequency 0, as upstream does.
  TEST_ASSERT_EQUAL_STRING(
      "{\"event\":\"detection\",\"detection_method\":\"ble_name\",\"detection_tier\":2,"
      "\"protocol\":\"ble\",\"mac_address\":\"d4:01:02:03:04:05\",\"oui\":\"d4:01:02\","
      "\"device_name\":\"Penguin-1234567890\",\"rssi\":-71,\"channel\":255,\"frequency\":0,"
      "\"ssid\":\"\",\"rule\":\"ble_name_penguin\",\"strength\":\"specific\"}",
      line);
}

void test_dashboard_json_sanitizes_name() {
  const Match m = make_match(Rule::BleNameDfuTarg, kOtherMac, "a\"b\\c\x01\xc3");
  char line[400];
  fy::format_dashboard_detection(m, -80, 0, line, sizeof(line));
  TEST_ASSERT_NOT_NULL(strstr(line, "\"device_name\":\"a?b?c??\","));
  TEST_ASSERT_NOT_NULL(strstr(line, "\"strength\":\"possible\"}"));
}

void test_dashboard_json_frequencies() {
  const Match m = make_match(Rule::WifiOuiTransmitter, kQualcommMac, "");
  char line[400];
  const struct {
    uint8_t channel;
    const char *expect;
  } cases[] = {{1, "\"frequency\":2412,"},
               {13, "\"frequency\":2472,"},
               {14, "\"frequency\":2484,"},
               {0, "\"frequency\":0,"}};
  for (const auto &c : cases) {
    fy::format_dashboard_detection(m, -60, c.channel, line, sizeof(line));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, c.expect), c.expect);
  }
}

void test_dashboard_json_small_buffer() {
  const Match m = make_match(Rule::WifiOuiBssid, kFlockMac, "");
  char line[40];
  TEST_ASSERT_EQUAL_UINT(0, fy::format_dashboard_detection(m, -60, 6, line, sizeof(line)));
  TEST_ASSERT_EQUAL_STRING("", line);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_dashboard_methods_follow_upstream);
  RUN_TEST(test_dashboard_json_wifi_fingerprint);
  RUN_TEST(test_dashboard_json_ble_name);
  RUN_TEST(test_dashboard_json_sanitizes_name);
  RUN_TEST(test_dashboard_json_frequencies);
  RUN_TEST(test_dashboard_json_small_buffer);
  RUN_TEST(test_every_rule_has_a_device_type);
  RUN_TEST(test_oui_table_size_matches_upstream);
  RUN_TEST(test_primary_signature_is_built);
  RUN_TEST(test_probe_with_primary_signature_is_specific);
  RUN_TEST(test_primary_signature_survives_trailing_fcs);
  RUN_TEST(test_wildcard_probe_without_signature_is_possible);
  RUN_TEST(test_directed_probe_is_transmitter_oui_match);
  RUN_TEST(test_receiver_oui_on_data_frame);
  RUN_TEST(test_bssid_oui_on_management_frame);
  RUN_TEST(test_unrelated_frame_has_no_match);
  RUN_TEST(test_short_frame_is_rejected);
  RUN_TEST(test_corrupt_ies_do_not_crash);
  RUN_TEST(test_output_capacity_is_respected);
  RUN_TEST(test_penguin_serial_name_is_specific);
  RUN_TEST(test_penguin_name_needs_ten_digits);
  RUN_TEST(test_battery_name_is_case_insensitive);
  RUN_TEST(test_generic_names_are_possible);
  RUN_TEST(test_company_id_is_possible);
  RUN_TEST(test_flock_service_uuid_is_specific);
  RUN_TEST(test_nordic_dfu_service_is_possible);
  RUN_TEST(test_raven_service_range);
  RUN_TEST(test_specific_rule_wins_over_possible);
  RUN_TEST(test_malformed_ad_is_safe);
  RUN_TEST(test_unrelated_advert_has_no_match);
  return UNITY_END();
}
