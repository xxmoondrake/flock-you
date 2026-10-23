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
const uint8_t kFlockMac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, 0x03};     // Flock Safety's block
const uint8_t kLiteonMac[6] = {0x24, 0xb2, 0xb9, 0x01, 0x02, 0x03};    // Flock radio module
const uint8_t kQualcommMac[6] = {0x00, 0x03, 0x7f, 0x50, 0x00, 0x01};  // QCA9377 default
const uint8_t kAxonMac[6] = {0x00, 0x25, 0xdf, 0x01, 0x02, 0x03};      // Axon (specific)
const uint8_t kMotorolaMac[6] = {0x4c, 0xcc, 0x34, 0x01, 0x02, 0x03};  // Motorola (possible)
const uint8_t kOtherMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};     // nothing

void append(Bytes &b, std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes); }

void append_ie(Bytes &b, uint8_t id, std::initializer_list<uint8_t> payload) {
  b.push_back(id);
  b.push_back(static_cast<uint8_t>(payload.size()));
  b.insert(b.end(), payload);
}

void append_ssid(Bytes &b, const char *ssid) {
  b.push_back(0);
  b.push_back(static_cast<uint8_t>(strlen(ssid)));
  b.insert(b.end(), ssid, ssid + strlen(ssid));
}

// 24-byte 802.11 header. fc0 0x40 = probe request, 0x50 = probe response,
// 0x80 = beacon, 0x08 = data.
Bytes header(uint8_t fc0, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3) {
  Bytes b = {fc0, 0x00, 0x00, 0x00};
  b.insert(b.end(), a1, a1 + 6);
  b.insert(b.end(), a2, a2 + 6);
  b.insert(b.end(), a3, a3 + 6);
  append(b, {0x00, 0x00});
  return b;
}

Bytes beacon(const uint8_t *bssid, const char *ssid) {
  Bytes f = header(0x80, kBroadcast, bssid, bssid);
  f.insert(f.end(), 12, 0x00);  // timestamp, interval, capability
  append_ssid(f, ssid);
  return f;
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

Bytes wildcard_probe(const uint8_t *transmitter) {
  Bytes f = header(0x40, kBroadcast, transmitter, kBroadcast);
  append_ie(f, 0, {});
  append_ie(f, 1, {0x82, 0x84});
  return f;
}

size_t match_frame(const Bytes &f, bool is_mgmt, Match *out) {
  return fy::match_wifi_frame(f.data(), f.size(), is_mgmt, out, fy::kMaxWifiMatchesPerFrame);
}

bool candidate(const Bytes &f, bool is_mgmt) {
  return fy::wifi_frame_is_candidate(f.data(), f.size(), is_mgmt);
}

Bytes ad_name(const char *name, uint8_t type = 0x09) {
  Bytes b;
  b.push_back(static_cast<uint8_t>(strlen(name) + 1));
  b.push_back(type);
  b.insert(b.end(), name, name + strlen(name));
  return b;
}

// A 16-bit UUID in Bluetooth-base-UUID 128-bit form, little-endian on air.
Bytes ad_uuid128_base(uint16_t short_uuid) {
  return {0x11, 0x07, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10,
          0x00, 0x00, static_cast<uint8_t>(short_uuid), static_cast<uint8_t>(short_uuid >> 8),
          0x00, 0x00};
}

bool match_ad(const Bytes &ad, Match *out, const uint8_t *addr = kOtherMac, bool public_addr = false) {
  return fy::match_ble_advert(addr, public_addr, ad.data(), ad.size(), out);
}

}  // namespace

void setUp() { fy::set_options(fy::Options{}); }
void tearDown() {}

// ---- Tables ------------------------------------------------------------------

void test_table_sizes() {
  // Upstream's 34 prefixes minus b8:35:32, which IEEE never assigned.
  TEST_ASSERT_EQUAL_UINT(33, fy::flock_radio_oui_count());
  // The parity reference's 78 watchlist rows.
  TEST_ASSERT_EQUAL_UINT(78, fy::watchlist_count());
}

void test_watchlist_entries_are_consistent() {
  // One address in each kind of entry classifies to a type whose possible
  // flag matches the entry's strength.
  const uint8_t probes[][6] = {
      {0x00, 0x25, 0xdf, 0, 0, 0}, {0x08, 0x3c, 0x03, 0x05, 0, 0}, {0x1c, 0x82, 0x59, 0xd1, 0, 0},
      {0x58, 0xe8, 0x76, 0xc2, 0, 0}, {0x6c, 0x18, 0x11, 0, 0, 0}, {0x38, 0x1d, 0x14, 0, 0, 0},
      {0xd4, 0x11, 0xd6, 0, 0, 0}, {0x00, 0x14, 0x3e, 0, 0, 0}, {0x00, 0x30, 0x7e, 0, 0, 0},
      {0x00, 0x00, 0xc3, 0, 0, 0}, {0x00, 0x08, 0xb8, 0, 0, 0}, {0xe4, 0x1e, 0x0a, 0xb3, 0, 0},
  };
  for (const auto &mac : probes) {
    const fy::OuiEntry *e = fy::watchlist_lookup(mac);
    TEST_ASSERT_NOT_NULL(e);
    const fy::Classification c = fy::classify(Rule::WifiOuiTransmitter, mac);
    TEST_ASSERT_EQUAL(e->strength, c.strength);
    TEST_ASSERT_EQUAL(e->strength == Strength::Possible, fy::device_type_info(c.type).possible);
    TEST_ASSERT_EQUAL_STRING(e->vendor, c.vendor);
  }
}

void test_watchlist_block_sizes() {
  const uint8_t federal_in[6] = {0x08, 0x3c, 0x03, 0x0f, 0xff, 0xff};   // MA-M 08:3C:03:0
  const uint8_t federal_out[6] = {0x08, 0x3c, 0x03, 0x10, 0x00, 0x00};
  const uint8_t genetec_in[6] = {0x00, 0x50, 0xc2, 0xbe, 0x7f, 0xff};   // IAB 00:50:C2:BE:7
  const uint8_t genetec_out[6] = {0x00, 0x50, 0xc2, 0xbe, 0x80, 0x00};
  TEST_ASSERT_EQUAL_STRING("Federal Signal", fy::watchlist_lookup(federal_in)->vendor);
  TEST_ASSERT_NULL(fy::watchlist_lookup(federal_out));
  TEST_ASSERT_EQUAL_STRING("Genetec", fy::watchlist_lookup(genetec_in)->vendor);
  TEST_ASSERT_NULL(fy::watchlist_lookup(genetec_out));
}

void test_name_collisions_are_not_watched() {
  // WatchGuard Technologies firewalls, Axon Networks, Vutility, Coban SRL.
  const uint8_t macs[][6] = {{0x00, 0x01, 0x21, 1, 2, 3}, {0x00, 0x90, 0x7f, 1, 2, 3},
                             {0x00, 0x58, 0x28, 1, 2, 3}, {0x84, 0x70, 0x03, 1, 2, 3},
                             {0xd4, 0x63, 0x52, 1, 2, 3}, {0x24, 0xa3, 0xf0, 0x70, 2, 3}};
  for (const auto &mac : macs) TEST_ASSERT_NULL(fy::watchlist_lookup(mac));
}

void test_every_rule_classifies() {
  for (int r = 0; r < static_cast<int>(Rule::Count); r++) {
    const fy::Classification c = fy::classify(static_cast<Rule>(r), kOtherMac);
    TEST_ASSERT_TRUE(static_cast<int>(c.type) < static_cast<int>(fy::DeviceType::Count));
    const fy::DeviceTypeInfo &info = fy::device_type_info(c.type);
    TEST_ASSERT_TRUE(strlen(info.name) <= 12);
    TEST_ASSERT_TRUE(info.label && info.label[0]);
    // A specific match never lands in a possible-only type.
    TEST_ASSERT_EQUAL(c.strength == Strength::Possible, info.possible);
  }
  TEST_ASSERT_EQUAL(fy::DeviceType::FlockCamera, fy::classify(Rule::WifiProbeIeSignature, kLiteonMac).type);
  TEST_ASSERT_EQUAL_STRING("Lite-On", fy::classify(Rule::WifiProbeIeSignature, kLiteonMac).vendor);
  TEST_ASSERT_EQUAL(fy::DeviceType::FlockRaven, fy::classify(Rule::BleServiceRaven, kOtherMac).type);
  TEST_ASSERT_EQUAL(fy::DeviceType::BodyCamera, fy::classify(Rule::BleOui, kAxonMac).type);
  TEST_ASSERT_EQUAL(fy::DeviceType::RadioPossible, fy::classify(Rule::WifiOuiReceiver, kMotorolaMac).type);
  // Records of radio-module OUIs from earlier builds stay possible Flock.
  TEST_ASSERT_EQUAL(fy::DeviceType::FlockPossible, fy::classify(Rule::WifiOuiTransmitter, kLiteonMac).type);
}

// ---- Wi-Fi -----------------------------------------------------------------

void test_primary_signature_is_built() {
  Bytes body;
  append_primary_ies(body);
  char sig[128];
  TEST_ASSERT_TRUE(fy::build_probe_ie_signature(body.data(), body.size(), sig, sizeof(sig)));
  TEST_ASSERT_EQUAL_STRING("2,12,127,221:506f9a16030103,45,191,221:0050f208000000", sig);
}

void test_probe_with_primary_signature_is_specific() {
  Match m[4];
  for (const uint8_t *mac : {kLiteonMac, kFlockMac}) {
    const Bytes f = primary_probe(mac);
    TEST_ASSERT_TRUE(candidate(f, true));
    TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
    TEST_ASSERT_EQUAL(Rule::WifiProbeIeSignature, m[0].rule);
    TEST_ASSERT_EQUAL(Strength::Specific, m[0].strength);
    TEST_ASSERT_EQUAL_MEMORY(mac, m[0].mac, 6);
  }
}

void test_primary_signature_survives_trailing_fcs() {
  Match m[4];
  Bytes f = primary_probe(kLiteonMac);
  append(f, {0xde, 0xad, 0xbe, 0xef});
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiProbeIeSignature, m[0].rule);
}

void test_wildcard_probe_without_signature_is_possible() {
  Match m[4];
  const Bytes f = wildcard_probe(kLiteonMac);
  TEST_ASSERT_TRUE(candidate(f, true));
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiWildcardProbe, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m[0].strength);
}

void test_flock_block_without_fingerprint_is_its_own_specific_match() {
  Match m[4];
  TEST_ASSERT_EQUAL_UINT(1, match_frame(wildcard_probe(kFlockMac), true, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiTransmitter, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m[0].strength);
}

void test_radio_module_ouis_only_in_probes_by_default() {
  Match m[4];
  Bytes directed = header(0x40, kBroadcast, kLiteonMac, kBroadcast);
  append_ssid(directed, "home");
  const Bytes data = header(0x08, kQualcommMac, kLiteonMac, kQualcommMac);
  // The cheap pre-filter passes every probe from these OUIs; matching drops
  // the directed ones.
  TEST_ASSERT_TRUE(candidate(directed, true));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(directed, true, m));
  TEST_ASSERT_FALSE(candidate(data, false));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(data, false, m));

  // Upstream's behaviour, as an option: any frame, labeled possible.
  fy::Options o;
  o.radio_ouis_any_frame = true;
  fy::set_options(o);
  TEST_ASSERT_TRUE(candidate(data, false));
  TEST_ASSERT_EQUAL_UINT(2, match_frame(data, false, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiTransmitter, m[0].rule);
  TEST_ASSERT_EQUAL(Rule::WifiOuiReceiver, m[1].rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m[0].strength);
}

void test_unassigned_upstream_prefix_is_gone() {
  Match m[4];
  fy::Options o;
  o.radio_ouis_any_frame = true;
  fy::set_options(o);
  const uint8_t mac[6] = {0xb8, 0x35, 0x32, 0x01, 0x02, 0x03};
  TEST_ASSERT_EQUAL_UINT(0, match_frame(header(0x08, mac, kOtherMac, mac), false, m));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(wildcard_probe(mac), true, m));
}

void test_watchlist_vendor_strengths() {
  Match m[4];
  const Bytes axon = header(0x08, kOtherMac, kAxonMac, kOtherMac);
  TEST_ASSERT_TRUE(candidate(axon, false));
  TEST_ASSERT_EQUAL_UINT(1, match_frame(axon, false, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiTransmitter, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m[0].strength);

  const Bytes moto = header(0x08, kMotorolaMac, kOtherMac, kOtherMac);
  TEST_ASSERT_EQUAL_UINT(1, match_frame(moto, false, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiReceiver, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m[0].strength);
}

void test_vendor_groups_switch_off_and_on() {
  Match m[4];
  const uint8_t cradlepoint[6] = {0x00, 0x30, 0x44, 0x01, 0x02, 0x03};
  const uint8_t sierra[6] = {0x00, 0xa0, 0xd5, 0x01, 0x02, 0x03};
  const Bytes cp = header(0x08, kOtherMac, cradlepoint, kOtherMac);
  const Bytes sw = header(0x08, kOtherMac, sierra, kOtherMac);
  const Bytes moto = header(0x08, kOtherMac, kMotorolaMac, kOtherMac);
  TEST_ASSERT_EQUAL_UINT(0, match_frame(cp, false, m));  // off by default
  TEST_ASSERT_EQUAL_UINT(1, match_frame(sw, false, m));
  TEST_ASSERT_EQUAL_UINT(1, match_frame(moto, false, m));

  fy::Options o;
  o.cradlepoint = true;
  o.sierra_wireless = false;
  o.motorola = false;
  fy::set_options(o);
  TEST_ASSERT_EQUAL_UINT(1, match_frame(cp, false, m));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(sw, false, m));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(moto, false, m));
  TEST_ASSERT_FALSE(candidate(moto, false));
  // Stored records still classify by vendor.
  TEST_ASSERT_EQUAL_STRING("Motorola Solutions", fy::classify(Rule::WifiOuiTransmitter, kMotorolaMac).vendor);
}

void test_bssid_oui_on_management_frame() {
  Match m[4];
  const Bytes f = header(0x80, kBroadcast, kOtherMac, kFlockMac);  // beacon, randomized addr2
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiOuiBssid, m[0].rule);
  // addr3 is only checked on management frames.
  TEST_ASSERT_EQUAL_UINT(0, match_frame(f, false, m));
}

void test_flock_softap_ssid() {
  Match m[4];
  const Bytes f = beacon(kOtherMac, "Flock-A1B2C3");
  TEST_ASSERT_TRUE(candidate(f, true));
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiSsidFlock, m[0].rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m[0].strength);
  TEST_ASSERT_EQUAL_STRING("Flock-A1B2C3", m[0].name);
  TEST_ASSERT_EQUAL_MEMORY(kOtherMac, m[0].mac, 6);

  TEST_ASSERT_EQUAL_UINT(1, match_frame(beacon(kOtherMac, "flock-abcd"), true, m));
  for (const char *ssid : {"Flock-Party", "Flock-12", "Flock-12345g", "MyFlock-ABCDEF", "Flock"}) {
    const Bytes no = beacon(kOtherMac, ssid);
    TEST_ASSERT_FALSE_MESSAGE(candidate(no, true), ssid);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, match_frame(no, true, m), ssid);
  }
  // A probe request for the network, and a probe response from it.
  Bytes probe = header(0x40, kBroadcast, kOtherMac, kBroadcast);
  append_ssid(probe, "Flock-0FEED1");
  TEST_ASSERT_EQUAL_UINT(1, match_frame(probe, true, m));
  Bytes response = header(0x50, kOtherMac, kOtherMac, kOtherMac);
  response.insert(response.end(), 12, 0x00);
  append_ssid(response, "Flock-0FEED1");
  TEST_ASSERT_EQUAL_UINT(1, match_frame(response, true, m));
}

void test_unrelated_frame_has_no_match() {
  Match m[4];
  Bytes f = header(0x40, kBroadcast, kOtherMac, kBroadcast);
  append_primary_ies(f);
  TEST_ASSERT_FALSE(candidate(f, true));
  TEST_ASSERT_EQUAL_UINT(0, match_frame(f, true, m));
}

void test_short_frame_is_rejected() {
  Match m[4];
  const Bytes f = primary_probe(kLiteonMac);
  TEST_ASSERT_FALSE(fy::wifi_frame_is_candidate(f.data(), 23, true));
  TEST_ASSERT_EQUAL_UINT(0, fy::match_wifi_frame(f.data(), 23, true, m, 4));
  const Bytes b = beacon(kOtherMac, "Flock-A1B2C3");
  TEST_ASSERT_EQUAL_UINT(0, fy::match_wifi_frame(b.data(), b.size() - 1, true, m, 4));  // SSID cut
}

void test_corrupt_ies_do_not_crash() {
  Match m[4];
  Bytes f = header(0x40, kBroadcast, kLiteonMac, kBroadcast);
  append(f, {0x00, 0x00, 0x2d, 0xff, 0x01});  // IE length overruns the frame
  TEST_ASSERT_EQUAL_UINT(1, match_frame(f, true, m));
  TEST_ASSERT_EQUAL(Rule::WifiWildcardProbe, m[0].rule);
}

void test_output_capacity_is_respected() {
  Match m[1];
  const Bytes f = header(0x80, kAxonMac, kFlockMac, kFlockMac);
  TEST_ASSERT_EQUAL_UINT(1, fy::match_wifi_frame(f.data(), f.size(), true, m, 1));
  TEST_ASSERT_EQUAL(Rule::WifiOuiTransmitter, m[0].rule);
}

// ---- BLE -------------------------------------------------------------------

void test_penguin_serial_name_is_specific() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("Penguin-0123456789"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNamePenguinSerial, m.rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m.strength);
  TEST_ASSERT_EQUAL_STRING("Penguin-0123456789", m.name);
  TEST_ASSERT_EQUAL_MEMORY(kOtherMac, m.mac, 6);
  TEST_ASSERT_FALSE(match_ad(ad_name("Penguin-12345"), &m));
  TEST_ASSERT_FALSE(match_ad(ad_name("Penguin-0123456789x"), &m));
}

void test_battery_name_is_case_insensitive() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("fs EXT battery", 0x08), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameExtBattery, m.rule);
}

void test_bare_serial_name_is_retired() {
  Match m;
  TEST_ASSERT_FALSE(match_ad(ad_name("0123456789"), &m));
  TEST_ASSERT_FALSE(match_ad(ad_name("0102000000"), &m));  // the phone that matched it
}

void test_loose_flock_names_are_possible() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("FS-BEC46A"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameFsHex, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m.strength);
  TEST_ASSERT_FALSE(match_ad(ad_name("FS-100"), &m));   // white-label gadget
  TEST_ASSERT_FALSE(match_ad(ad_name("FS-12G4"), &m));  // not hex
  TEST_ASSERT_TRUE(match_ad(ad_name("Flock Thing"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameFlock, m.rule);
  TEST_ASSERT_TRUE(match_ad(ad_name("DfuTarg"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameDfuTarg, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m.strength);
}

void test_company_ids() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(Bytes{0x05, 0xff, 0xc8, 0x09, 0x01, 0x02}, &m));
  TEST_ASSERT_EQUAL(Rule::BleMfgXuntong, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m.strength);
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0xff, 0x4d, 0x03}, &m));
  TEST_ASSERT_EQUAL(Rule::BleMfgAxon, m.rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m.strength);
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0xff, 0xec, 0x04}, &m));
  TEST_ASSERT_EQUAL(Rule::BleMfgMotorola, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m.strength);
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

void test_raven_services_exact() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(Bytes{0x05, 0x03, 0x0f, 0x18, 0x00, 0x31}, &m));  // 0x180f, 0x3100
  TEST_ASSERT_EQUAL(Rule::BleServiceRaven, m.rule);
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0x02, 0x00, 0x35}, &m));  // 0x3500
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0x03, 0x01, 0x31}, &m));  // 0x3101: a characteristic
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0x03, 0x50, 0x32}, &m));  // 0x3250
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0x03, 0x00, 0x36}, &m));  // 0x3600
  // Raven advertises the shorts in 128-bit base-UUID form.
  TEST_ASSERT_TRUE(match_ad(ad_uuid128_base(0x3300), &m));
  TEST_ASSERT_EQUAL(Rule::BleServiceRaven, m.rule);
}

void test_axon_tag_in_either_byte_order() {
  Match m;
  // "AXJANUSBWCDEVICE" as a 128-bit UUID: reversed on air.
  const Bytes reversed = {0x11, 0x07, 'E', 'C', 'I', 'V', 'E', 'D', 'C', 'W',
                          'B', 'S', 'U', 'N', 'A', 'J', 'X', 'A'};
  TEST_ASSERT_TRUE(match_ad(reversed, &m));
  TEST_ASSERT_EQUAL(Rule::BleAxonTag, m.rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m.strength);
  // Forward, in 16-bit service data.
  const Bytes forward = {0x0c, 0x16, 0x6b, 0xfe, 'B', 'W', 'C', 'D', 'E', 'V', 'I', 'C', 'E'};
  TEST_ASSERT_TRUE(match_ad(forward, &m));
  TEST_ASSERT_EQUAL(Rule::BleAxonTag, m.rule);
  // The spaced display form never appears on air; a name is not service data.
  const Bytes spaced = {0x0d, 0x16, 0x00, 0x00, 'B', 'W', 'C', ' ', 'D', 'E', 'V', 'I', 'C', 'E'};
  TEST_ASSERT_FALSE(match_ad(spaced, &m));
  TEST_ASSERT_FALSE(match_ad(ad_name("BWCDEVICE"), &m));
}

void test_axon_and_motorola_services() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(Bytes{0x05, 0x16, 0x6b, 0xfe, 0x01, 0x02}, &m));  // svc data 0xFE6B
  TEST_ASSERT_EQUAL(Rule::BleServiceAxon, m.rule);
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0x03, 0x81, 0xfc}, &m));  // 0xFC81
  TEST_ASSERT_EQUAL(Rule::BleServiceAxon, m.rule);
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0x03, 0x8e, 0xfd}, &m));  // 0xFD8E
  TEST_ASSERT_EQUAL(Rule::BleServiceMotorola, m.rule);
  TEST_ASSERT_EQUAL(Strength::Possible, m.strength);

  fy::Options o;
  o.motorola = false;
  fy::set_options(o);
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0x03, 0x8e, 0xfd}, &m));
  TEST_ASSERT_FALSE(match_ad(Bytes{0x03, 0xff, 0xec, 0x04}, &m));
  TEST_ASSERT_FALSE(match_ad(Bytes{0x02, 0x01, 0x06}, &m, kMotorolaMac, true));
}

void test_bodyworn_remote_name() {
  Match m;
  TEST_ASSERT_TRUE(match_ad(ad_name("BodyWorn Remote 0042"), &m));
  TEST_ASSERT_EQUAL(Rule::BleNameBodyWorn, m.rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m.strength);
  TEST_ASSERT_FALSE(match_ad(ad_name("BodyWorn"), &m));
}

void test_ble_oui_only_on_public_addresses() {
  Match m;
  const Bytes flags = {0x02, 0x01, 0x06};
  TEST_ASSERT_TRUE(match_ad(flags, &m, kAxonMac, true));
  TEST_ASSERT_EQUAL(Rule::BleOui, m.rule);
  TEST_ASSERT_EQUAL(Strength::Specific, m.strength);
  TEST_ASSERT_FALSE(match_ad(flags, &m, kAxonMac, false));  // random: OUI means nothing
  TEST_ASSERT_TRUE(match_ad(flags, &m, kMotorolaMac, true));
  TEST_ASSERT_EQUAL(Strength::Possible, m.strength);
  TEST_ASSERT_FALSE(match_ad(flags, &m, kLiteonMac, true));  // radio modules: Wi-Fi probes only
}

void test_specific_rule_wins_over_possible() {
  Match m;
  Bytes ad = ad_name("DfuTarg");
  append(ad, {0x05, 0x03, 0x00, 0x32, 0x0f, 0x18});  // Raven 0x3200
  TEST_ASSERT_TRUE(match_ad(ad, &m));
  TEST_ASSERT_EQUAL(Rule::BleServiceRaven, m.rule);
  TEST_ASSERT_EQUAL_STRING("DfuTarg", m.name);
  // A Motorola advert from an Axon public address is an Axon match.
  TEST_ASSERT_TRUE(match_ad(Bytes{0x03, 0xff, 0xec, 0x04}, &m, kAxonMac, true));
  TEST_ASSERT_EQUAL(Rule::BleOui, m.rule);
}

void test_malformed_ad_is_safe() {
  Match m;
  TEST_ASSERT_FALSE(match_ad(Bytes{0x1f, 0x09, 'P', 'e'}, &m));  // length overruns
  TEST_ASSERT_FALSE(match_ad(Bytes{0x00, 0x09, 0x41}, &m));      // zero-length field
  TEST_ASSERT_FALSE(match_ad(Bytes{0x01}, &m));                  // truncated header
  TEST_ASSERT_FALSE(match_ad(Bytes{0x05, 0x21, 'B', 'W', 'C', 'D'}, &m));  // short service data
  TEST_ASSERT_FALSE(fy::match_ble_advert(kOtherMac, false, nullptr, 0, &m));
}

void test_unrelated_advert_has_no_match() {
  Match m;
  Bytes ad = {0x02, 0x01, 0x06};
  const Bytes name = ad_name("Android");
  ad.insert(ad.end(), name.begin(), name.end());
  TEST_ASSERT_FALSE(match_ad(ad, &m));
  TEST_ASSERT_FALSE(match_ad(ad_uuid128_base(0x180f), &m));
}

// ---- Upstream dashboard JSON ------------------------------------------------

Match make_match(Rule rule, const uint8_t *mac, const char *name) {
  Match m = {};
  m.rule = rule;
  m.strength = fy::rule_info(rule).strength;
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
  TEST_ASSERT_EQUAL_STRING("wifi_ssid", fy::rule_info(Rule::WifiSsidFlock).upstream_method);
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

void test_dashboard_json_ssid_and_vendor_strength() {
  char line[400];
  fy::format_dashboard_detection(make_match(Rule::WifiSsidFlock, kOtherMac, "Flock-A1B2C3"), -60, 6,
                                 line, sizeof(line));
  TEST_ASSERT_NOT_NULL(strstr(line, "\"detection_method\":\"wifi_ssid\""));
  TEST_ASSERT_NOT_NULL(strstr(line, "\"device_name\":\"\""));
  TEST_ASSERT_NOT_NULL(strstr(line, "\"ssid\":\"Flock-A1B2C3\""));
  Match axon = make_match(Rule::WifiOuiTransmitter, kAxonMac, "");
  axon.strength = Strength::Specific;  // the vendor's, not the rule's default
  fy::format_dashboard_detection(axon, -60, 6, line, sizeof(line));
  TEST_ASSERT_NOT_NULL(strstr(line, "\"strength\":\"specific\"}"));
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
  RUN_TEST(test_table_sizes);
  RUN_TEST(test_watchlist_entries_are_consistent);
  RUN_TEST(test_watchlist_block_sizes);
  RUN_TEST(test_name_collisions_are_not_watched);
  RUN_TEST(test_every_rule_classifies);
  RUN_TEST(test_primary_signature_is_built);
  RUN_TEST(test_probe_with_primary_signature_is_specific);
  RUN_TEST(test_primary_signature_survives_trailing_fcs);
  RUN_TEST(test_wildcard_probe_without_signature_is_possible);
  RUN_TEST(test_flock_block_without_fingerprint_is_its_own_specific_match);
  RUN_TEST(test_radio_module_ouis_only_in_probes_by_default);
  RUN_TEST(test_unassigned_upstream_prefix_is_gone);
  RUN_TEST(test_watchlist_vendor_strengths);
  RUN_TEST(test_vendor_groups_switch_off_and_on);
  RUN_TEST(test_bssid_oui_on_management_frame);
  RUN_TEST(test_flock_softap_ssid);
  RUN_TEST(test_unrelated_frame_has_no_match);
  RUN_TEST(test_short_frame_is_rejected);
  RUN_TEST(test_corrupt_ies_do_not_crash);
  RUN_TEST(test_output_capacity_is_respected);
  RUN_TEST(test_penguin_serial_name_is_specific);
  RUN_TEST(test_battery_name_is_case_insensitive);
  RUN_TEST(test_bare_serial_name_is_retired);
  RUN_TEST(test_loose_flock_names_are_possible);
  RUN_TEST(test_company_ids);
  RUN_TEST(test_flock_service_uuid_is_specific);
  RUN_TEST(test_nordic_dfu_service_is_possible);
  RUN_TEST(test_raven_services_exact);
  RUN_TEST(test_axon_tag_in_either_byte_order);
  RUN_TEST(test_axon_and_motorola_services);
  RUN_TEST(test_bodyworn_remote_name);
  RUN_TEST(test_ble_oui_only_on_public_addresses);
  RUN_TEST(test_specific_rule_wins_over_possible);
  RUN_TEST(test_malformed_ad_is_safe);
  RUN_TEST(test_unrelated_advert_has_no_match);
  RUN_TEST(test_dashboard_methods_follow_upstream);
  RUN_TEST(test_dashboard_json_wifi_fingerprint);
  RUN_TEST(test_dashboard_json_ble_name);
  RUN_TEST(test_dashboard_json_ssid_and_vendor_strength);
  RUN_TEST(test_dashboard_json_sanitizes_name);
  RUN_TEST(test_dashboard_json_frequencies);
  RUN_TEST(test_dashboard_json_small_buffer);
  return UNITY_END();
}
