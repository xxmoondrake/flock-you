// SPDX-License-Identifier: MIT
//
// Signature data and matching logic. The Flock rules and the probe-request
// fingerprint are ported from the upstream flock-you firmware (main.cpp at
// commit 6704f01), Copyright (c) 2026 colonelpanichacks, MIT License; see
// LICENSE in the repository root. The vendor watchlist and the Axon, BodyWorn,
// Motorola, and extra Flock identifiers follow the device families of
// datafx/alpr_detector (reviewed at 35ae012); only identifiers were taken,
// each checked against the IEEE or Bluetooth SIG registries or a published
// field capture. Sources and the review are in
// datasets/esp_idf_firmware_signatures.md.
//
// Differences from upstream, by design:
// - Matching runs in a task, not in the radio callbacks. The Wi-Fi callback
//   only calls wifi_frame_is_candidate().
// - Each match carries a strength (specific or possible) in addition to the
//   upstream tier.
// - BLE adverts are parsed from raw AD structures instead of NimBLE-Arduino
//   objects, and a specific BLE rule wins over a possible one.
// - The SSID keyword path (disabled upstream) is not ported; only the
//   anchored "Flock-" SoftAP pattern is matched.

#include "fy_signatures.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace fy {
namespace {

// ---------------------------------------------------------------------------
// Rule metadata
// ---------------------------------------------------------------------------

// Upstream methods and tiers are those its firmware emits for the same
// signature (alertTypeToMethod and alertTypeToTier in upstream main.cpp);
// rules upstream lacks use the closest method key its dashboard knows.
constexpr RuleInfo kRuleInfo[] = {
    {"wifi_probe_ie_sig", Transport::Wifi, Strength::Specific, 4, "wifi_wildcard_probe_ie_sig"},
    {"wifi_wildcard_probe", Transport::Wifi, Strength::Possible, 3, "wifi_wildcard_probe"},
    {"wifi_oui_addr2", Transport::Wifi, Strength::Possible, 2, "wifi_oui_addr2"},
    {"wifi_oui_addr1", Transport::Wifi, Strength::Possible, 1, "wifi_oui_addr1"},
    {"wifi_oui_addr3", Transport::Wifi, Strength::Possible, 1, "wifi_oui_addr3"},
    {"ble_name_penguin", Transport::Ble, Strength::Specific, 2, "ble_name"},
    {"ble_name_fs_ext_battery", Transport::Ble, Strength::Specific, 2, "ble_name"},
    {"ble_name_bare_serial", Transport::Ble, Strength::Possible, 2, "ble_name"},
    {"ble_name_dfutarg", Transport::Ble, Strength::Possible, 2, "ble_name"},
    {"ble_mfg_09c8", Transport::Ble, Strength::Possible, 2, "ble_mfg"},
    {"ble_svc_flock", Transport::Ble, Strength::Specific, 2, "ble_gatt_svc"},
    {"ble_svc_nordic_dfu", Transport::Ble, Strength::Possible, 2, "ble_gatt_svc"},
    {"ble_svc_raven", Transport::Ble, Strength::Specific, 2, "ble_gatt_svc"},
    {"wifi_ssid_flock", Transport::Wifi, Strength::Specific, 3, "wifi_ssid"},
    {"ble_axon_tag", Transport::Ble, Strength::Specific, 2, "ble_gatt_svc"},
    {"ble_mfg_axon", Transport::Ble, Strength::Specific, 2, "ble_mfg"},
    {"ble_svc_axon", Transport::Ble, Strength::Specific, 2, "ble_gatt_svc"},
    {"ble_name_bodyworn", Transport::Ble, Strength::Specific, 2, "ble_name"},
    {"ble_mfg_motorola", Transport::Ble, Strength::Possible, 2, "ble_mfg"},
    {"ble_svc_motorola", Transport::Ble, Strength::Possible, 2, "ble_gatt_svc"},
    {"ble_name_fs_hex", Transport::Ble, Strength::Possible, 2, "ble_name"},
    {"ble_name_flock", Transport::Ble, Strength::Possible, 2, "ble_name"},
    {"ble_oui", Transport::Ble, Strength::Possible, 1, "ble_oui"},
};
static_assert(sizeof(kRuleInfo) / sizeof(kRuleInfo[0]) == static_cast<size_t>(Rule::Count),
              "every rule needs metadata");

// ---------------------------------------------------------------------------
// Vendor watchlist
// ---------------------------------------------------------------------------
//
// Every entry is an IEEE assignment checked against the live registry on
// 2026-10-22 (MA-L, MA-M, MA-S, and IAB lists), with the registrant named in
// `vendor`. The one exception is D8:1F:65: the registry lists its holder as
// "Private", and it is attributed to Axon by published field captures (nine
// devices, eight carrying Axon's BWCDEVICE tag). Remove it if a non-Axon
// device ever shows up on it.
//
// Specific: the vendor sells essentially only law-enforcement or
// surveillance gear, so its own block says something. Possible: real police
// suppliers whose blocks also ship on large volumes of civilian equipment.
// Name collisions are deliberately absent (WatchGuard Technologies firewalls,
// Coban SRL, TAIT Global LLC, Axon Networks, Vutility), as are blocks too
// broad to mean anything (Dell, Panasonic, Peplink/Pepwave).
using C = Category;
constexpr Strength S = Strength::Specific;
constexpr Strength P = Strength::Possible;
constexpr VendorGroup kNone = VendorGroup::None;
constexpr VendorGroup kMoto = VendorGroup::Motorola;
constexpr VendorGroup kSierra = VendorGroup::SierraWireless;
constexpr VendorGroup kCradle = VendorGroup::Cradlepoint;

constexpr OuiEntry kWatchlist[] = {
    {{0x00, 0x00, 0xc3}, 24, "Harris", C::Radio, P, kNone},
    {{0x00, 0x04, 0x4c}, 24, "Jenoptik", C::Alpr, P, kNone},
    {{0x00, 0x04, 0x7d}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0x00, 0x06, 0xec}, 24, "Harris", C::Radio, P, kNone},
    {{0x00, 0x08, 0xb8}, 24, "E.F. Johnson", C::Radio, S, kNone},
    {{0x00, 0x09, 0xbc}, 24, "Utility Inc. (BodyWorn)", C::BodyCamera, S, kNone},
    {{0x00, 0x0a, 0x3e}, 24, "EADS Telecom", C::Radio, P, kNone},
    {{0x00, 0x0d, 0x4f}, 24, "Kenwood", C::Radio, P, kNone},
    {{0x00, 0x0d, 0xca}, 24, "Tait Electronics", C::Radio, P, kNone},
    {{0x00, 0x0e, 0x06}, 24, "Team Simoco", C::Radio, P, kNone},
    {{0x00, 0x12, 0xe0}, 24, "Codan", C::Radio, P, kNone},
    {{0x00, 0x14, 0x3e}, 24, "AirLink Communications", C::VehicleNetwork, P, kNone},
    {{0x00, 0x14, 0x91}, 24, "Codan Radio", C::Radio, P, kNone},
    {{0x00, 0x16, 0xed}, 24, "Utility Inc. (BodyWorn)", C::BodyCamera, S, kNone},
    {{0x00, 0x17, 0x28}, 24, "Selex Communications", C::Radio, P, kNone},
    {{0x00, 0x17, 0x3d}, 24, "Neology", C::Alpr, P, kNone},
    {{0x00, 0x17, 0xf3}, 24, "Harris", C::Radio, P, kNone},
    {{0x00, 0x18, 0x85}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0x00, 0x1a, 0x08}, 24, "Simoco", C::Radio, P, kNone},
    {{0x00, 0x1c, 0x3c}, 24, "Seon Design", C::BodyCamera, S, kNone},
    {{0x00, 0x1d, 0x96}, 24, "WatchGuard Video", C::BodyCamera, S, kNone},
    {{0x00, 0x1e, 0x96}, 24, "Sepura", C::Radio, P, kNone},
    {{0x00, 0x1f, 0x92}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0x00, 0x1f, 0x9c}, 24, "Havis", C::VehicleNetwork, P, kNone},
    {{0x00, 0x22, 0xaf}, 24, "Safety Vision", C::BodyCamera, P, kNone},
    {{0x00, 0x23, 0xb9}, 24, "Airbus Defence and Space", C::Radio, P, kNone},
    {{0x00, 0x23, 0xbd}, 24, "Digital Ally", C::BodyCamera, S, kNone},
    {{0x00, 0x24, 0x39}, 24, "Digital Barriers", C::BodyCamera, S, kNone},
    {{0x00, 0x24, 0xe6}, 24, "In Motion Technology", C::VehicleNetwork, P, kNone},
    {{0x00, 0x25, 0xdf}, 24, "Axon", C::BodyCamera, S, kNone},
    {{0x00, 0x26, 0xb3}, 24, "Thales Communications", C::Radio, P, kNone},
    {{0x00, 0x30, 0x44}, 24, "Cradlepoint", C::VehicleNetwork, P, kCradle},
    {{0x00, 0x30, 0x7e}, 24, "Redflex", C::Alpr, S, kNone},
    {{0x00, 0x40, 0xde}, 24, "Elsag Datamat", C::Alpr, P, kNone},
    {{0x00, 0x50, 0xc2, 0xbe, 0x70}, 36, "Genetec", C::Alpr, P, kNone},
    {{0x00, 0x90, 0xc7}, 24, "Icom", C::Radio, P, kNone},
    {{0x00, 0xa0, 0xd5}, 24, "Sierra Wireless", C::VehicleNetwork, P, kSierra},
    {{0x00, 0xbf, 0x15}, 24, "Genetec", C::Alpr, P, kNone},
    {{0x00, 0xe0, 0x1c}, 24, "Cradlepoint", C::VehicleNetwork, P, kCradle},
    {{0x00, 0xe0, 0x6a}, 24, "Kapsch", C::Alpr, P, kNone},
    {{0x04, 0xc3, 0xe6, 0x90}, 28, "Ekin Teknoloji", C::Alpr, P, kNone},
    {{0x08, 0x3c, 0x03, 0x00}, 28, "Federal Signal", C::LightBar, S, kNone},
    {{0x0c, 0xbf, 0x15}, 24, "Genetec", C::Alpr, P, kNone},
    {{0x10, 0x74, 0x6f}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0x1c, 0x82, 0x59, 0xd0}, 28, "Applied Concepts (Stalker)", C::SpeedRadar, S, kNone},
    {{0x28, 0xa3, 0x31}, 24, "Sierra Wireless", C::VehicleNetwork, P, kSierra},
    {{0x38, 0x1d, 0x14}, 24, "Skydio", C::Drone, P, kNone},
    {{0x38, 0x73, 0xea, 0x00}, 28, "L-3 Mobile-Vision", C::BodyCamera, S, kNone},
    {{0x48, 0x46, 0x8d}, 24, "Zepcam", C::BodyCamera, S, kNone},
    {{0x48, 0xe3, 0xc3}, 24, "Jenoptik", C::Alpr, P, kNone},
    {{0x4c, 0xcc, 0x34}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0x50, 0x13, 0x9d}, 24, "Sierra Wireless", C::VehicleNetwork, P, kSierra},
    {{0x58, 0x94, 0xcf}, 24, "Vertex Standard LMR", C::Radio, P, kNone},
    {{0x58, 0xe8, 0x76, 0xc0}, 28, "Kustom Signals", C::SpeedRadar, S, kNone},
    {{0x64, 0x69, 0xbc}, 24, "Hytera", C::Radio, P, kNone},
    {{0x64, 0xce, 0x6e}, 24, "Sierra Wireless", C::VehicleNetwork, P, kSierra},
    {{0x68, 0xda, 0x73, 0xb0}, 28, "Gamber-Johnson", C::VehicleNetwork, P, kNone},
    {{0x6c, 0x18, 0x11}, 24, "Decatur Electronics", C::SpeedRadar, S, kNone},
    {{0x70, 0x1a, 0xd5}, 24, "Avigilon Alta", C::Alpr, P, kNone},
    {{0x70, 0xb3, 0xd5, 0x1c, 0x50}, 36, "ELSAG", C::Alpr, S, kNone},
    {{0x70, 0xb3, 0xd5, 0x52, 0x10}, 36, "Selex ES", C::Alpr, P, kNone},
    {{0x70, 0xb3, 0xd5, 0x88, 0xa0}, 36, "Perceptics", C::Alpr, S, kNone},
    {{0x70, 0xb3, 0xd5, 0xf5, 0xe0}, 36, "Selex ES", C::Alpr, P, kNone},
    {{0x84, 0xdb, 0x2f}, 24, "Sierra Wireless", C::VehicleNetwork, P, kSierra},
    {{0x94, 0x7b, 0xbe}, 24, "Ubicquia", C::Alpr, P, kNone},
    {{0x9c, 0x06, 0x6e}, 24, "Hytera", C::Radio, P, kNone},
    {{0x9c, 0x83, 0xbf}, 24, "PRO-VISION", C::BodyCamera, S, kNone},
    {{0x9c, 0x86, 0x2b}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0xb4, 0x1e, 0x52}, 24, "Flock Safety", C::Flock, S, kNone},
    {{0xb8, 0xe2, 0x8c}, 24, "Motorola Solutions", C::Radio, P, kMoto},
    {{0xbc, 0xad, 0x90}, 24, "Kymeta", C::VehicleNetwork, P, kNone},
    {{0xcc, 0x93, 0x4a}, 24, "Sierra Wireless", C::VehicleNetwork, P, kSierra},
    {{0xd4, 0x11, 0xd6}, 24, "ShotSpotter", C::GunshotSensor, S, kNone},
    {{0xd8, 0x1f, 0x65}, 24, "Axon (field-attributed)", C::BodyCamera, S, kNone},
    {{0xe0, 0xda, 0xdc}, 24, "JVC Kenwood", C::Radio, P, kNone},
    {{0xe4, 0x1e, 0x0a, 0xb0}, 28, "Safety Vision", C::BodyCamera, P, kNone},
    {{0xf4, 0x69, 0xd5, 0x70}, 28, "Rosco", C::BodyCamera, P, kNone},
    {{0xfc, 0x01, 0x9e}, 24, "VIEVU", C::BodyCamera, S, kNone},
};
constexpr size_t kWatchlistCount = sizeof(kWatchlist) / sizeof(kWatchlist[0]);

bool entry_covers(const OuiEntry &e, const uint8_t *mac) {
  if (mac[0] != e.prefix[0] || mac[1] != e.prefix[1] || mac[2] != e.prefix[2]) return false;
  if (e.bits == 24) return true;
  if (e.bits == 28) return (mac[3] & 0xf0) == (e.prefix[3] & 0xf0);
  return mac[3] == e.prefix[3] && (mac[4] & 0xf0) == (e.prefix[4] & 0xf0);
}

const OuiEntry *find_watchlist(const uint8_t *mac) {
  for (const OuiEntry &e : kWatchlist) {
    if (entry_covers(e, mac)) return &e;
  }
  return nullptr;
}

Options g_options;

bool group_enabled(VendorGroup group) {
  switch (group) {
    case VendorGroup::Motorola: return g_options.motorola;
    case VendorGroup::SierraWireless: return g_options.sierra_wireless;
    case VendorGroup::Cradlepoint: return g_options.cradlepoint;
    default: return true;
  }
}

// The watchlist entry for matching: null when its vendor group is off.
const OuiEntry *watched(const uint8_t *mac) {
  const OuiEntry *e = find_watchlist(mac);
  return e && group_enabled(e->group) ? e : nullptr;
}

// ---------------------------------------------------------------------------
// Flock radio-module OUIs
// ---------------------------------------------------------------------------
//
// From upstream: community field research (@NitekryDPaul, nite-oui-collection
// my_tested_flock.md, synced 2026-07-16), plus 82:6b:f2 from DeFlockJoplin's
// drive test and the Qualcomm Atheros default MAC block from the Flock
// camera firmware dump. Registrants checked against the IEEE registry on
// 2026-10-22. These are radio-module makers whose prefixes also ship in
// consumer laptops, phones, and IoT gear, so by default they are matched only
// in wildcard probe requests, where the IE fingerprint separates a camera
// from a laptop. Upstream's b8:35:32 is not an IEEE assignment and is left
// out; Flock Safety's own b4:1e:52 is also here so its probes get the
// fingerprint check.
struct RadioOui {
  uint8_t prefix[3];
  const char *vendor;
};

constexpr RadioOui kFlockRadioOuis[] = {
    {{0x70, 0xc9, 0x4e}, "Lite-On"}, {{0x3c, 0x91, 0x80}, "Lite-On"},
    {{0xd8, 0xf3, 0xbc}, "Lite-On"}, {{0x80, 0x30, 0x49}, "Lite-On"},
    {{0x14, 0x5a, 0xfc}, "Lite-On"}, {{0x74, 0x4c, 0xa1}, "Lite-On"},
    {{0x08, 0x3a, 0x88}, "USI"},     {{0x9c, 0x2f, 0x9d}, "Lite-On"},
    {{0xc0, 0x35, 0x32}, "Lite-On"}, {{0x94, 0x08, 0x53}, "Lite-On"},
    {{0xe4, 0xaa, 0xea}, "Lite-On"}, {{0xf4, 0x6a, 0xdd}, "Lite-On"},
    {{0xe0, 0x0a, 0xf6}, "Lite-On"}, {{0x24, 0xb2, 0xb9}, "Lite-On"},
    {{0x00, 0xf4, 0x8d}, "Lite-On"}, {{0xd0, 0x39, 0x57}, "Lite-On"},
    {{0xe8, 0xd0, 0xfc}, "Lite-On"}, {{0xe0, 0x4f, 0x43}, "USI"},
    {{0xb8, 0x1e, 0xa4}, "Lite-On"}, {{0x70, 0x08, 0x94}, "Lite-On"},
    {{0x58, 0x8e, 0x81}, "Silicon Labs"}, {{0xec, 0x1b, 0xbd}, "Silicon Labs"},
    {{0x3c, 0x71, 0xbf}, "Espressif"}, {{0x58, 0x00, 0xe3}, "Lite-On"},
    {{0x90, 0x35, 0xea}, "Silicon Labs"}, {{0x5c, 0x93, 0xa2}, "Lite-On"},
    {{0x64, 0x6e, 0x69}, "Lite-On"}, {{0x48, 0x27, 0xea}, "Samsung"},
    {{0xa4, 0xcf, 0x12}, "Espressif"}, {{0x14, 0xb5, 0xcd}, "Lite-On"},
    // Locally administered: never registry-assigned; one drive-tested camera.
    {{0x82, 0x6b, 0xf2}, "locally administered"},
    {{0xb4, 0x1e, 0x52}, "Flock Safety"},
    // Default MACs in the camera's QCA9377 radio firmware (firmware dump).
    {{0x00, 0x03, 0x7f}, "Qualcomm Atheros"},
};
constexpr size_t kFlockRadioOuiCount = sizeof(kFlockRadioOuis) / sizeof(kFlockRadioOuis[0]);

// No locally administered pre-filter: 82:6b:f2 has that bit set. Randomized
// phone MACs simply fail the comparison.
const RadioOui *find_radio_oui(const uint8_t *mac) {
  for (const RadioOui &r : kFlockRadioOuis) {
    if (mac[0] == r.prefix[0] && mac[1] == r.prefix[1] && mac[2] == r.prefix[2]) return &r;
  }
  return nullptr;
}

DeviceType type_for(Category category, Strength strength) {
  const bool specific = strength == Strength::Specific;
  switch (category) {
    case C::Flock: return specific ? DeviceType::FlockCamera : DeviceType::FlockPossible;
    case C::Alpr: return specific ? DeviceType::Alpr : DeviceType::AlprPossible;
    case C::BodyCamera: return specific ? DeviceType::BodyCamera : DeviceType::BodyCameraPossible;
    case C::Radio: return specific ? DeviceType::Radio : DeviceType::RadioPossible;
    case C::VehicleNetwork: return DeviceType::VehicleNetworkPossible;
    case C::SpeedRadar: return DeviceType::SpeedRadar;
    case C::LightBar: return DeviceType::LightBar;
    case C::Drone: return DeviceType::DronePossible;
    case C::GunshotSensor: return DeviceType::GunshotSensor;
  }
  return DeviceType::FlockPossible;
}

// Group bit set: multicast or broadcast, never a single device.
bool is_multicast(const uint8_t *mac) { return (mac[0] & 0x01) != 0; }

// ---------------------------------------------------------------------------
// 802.11 frame layout
// ---------------------------------------------------------------------------

constexpr size_t kMacHeaderLen = 24;
constexpr size_t kAddr1Offset = 4;
constexpr size_t kAddr2Offset = 10;
constexpr size_t kAddr3Offset = 16;
constexpr size_t kFcsLen = 4;

bool is_probe_request(const uint8_t *frame) {
  const uint8_t fc0 = frame[0];
  const uint8_t type = (fc0 >> 2) & 0x03;
  const uint8_t subtype = (fc0 >> 4) & 0x0f;
  return type == 0 && subtype == 4;
}

// Returns 1 for a wildcard SSID IE (tag 0, length 0), 0 for a directed probe
// (non-empty SSID), and -1 when no SSID IE was found.
int wildcard_ssid_state(const uint8_t *body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    const uint8_t id = body[0];
    const uint8_t elen = body[1];
    if (static_cast<int>(elen) + 2 > len) break;
    if (id == 0) return elen == 0 ? 1 : 0;
    body += elen + 2;
    len -= elen + 2;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Probe-request IE fingerprint
// ---------------------------------------------------------------------------
//
// Community/LiteON-derived fingerprint (@NitekryDPaul / DeFlockJoplin drive
// tests), kept upstream as half of the signature union because it is the
// highest-precision Wi-Fi signature fielded against LiteON-built cameras.

constexpr char kPrimaryIeSignature[] = "2,12,127,221:506f9a16030103,45,191,221:0050f208000000";
constexpr char kLiteonIePrefix[] = "221:506f9a16030103";

constexpr uint8_t kIeSsid = 0;
constexpr uint8_t kIeVendor = 221;
constexpr int kPhantomSkipCap = 16;
constexpr int kTlvResyncMax = 64;
constexpr size_t kSignatureCap = 128;

void hex_bytes(char *dst, const uint8_t *b, int n) {
  static const char hd[] = "0123456789abcdef";
  for (int i = 0; i < n; i++) {
    dst[i * 2] = hd[b[i] >> 4];
    dst[i * 2 + 1] = hd[b[i] & 0x0f];
  }
}

// True when ies[pos] starts vendor IE 221 with OUI 50:6f:9a (LiteON / Flock
// stack). Used to find real IE boundaries inside corrupted TLV runs.
bool liteon_vendor_at(const uint8_t *ies, int len, int pos) {
  return pos + 9 <= len && ies[pos] == kIeVendor && ies[pos + 1] == 7 && ies[pos + 2] == 0x50 &&
         ies[pos + 3] == 0x6f && ies[pos + 4] == 0x9a;
}

// Looks up to 32 bytes past a bogus TLV header for a real LiteON vendor IE,
// which signals a phantom overflow (driver length/FCS skew) rather than the
// end of the frame.
bool phantom_liteon_ahead(const uint8_t *ies, int len, int pos) {
  int end = pos + 2 + 32;
  if (end > len - 1) end = len - 1;
  for (int j = pos + 2; j < end; j++) {
    if (liteon_vendor_at(ies, len, j)) return true;
  }
  return false;
}

bool is_phantom_overflow(const uint8_t *ies, int len, uint8_t id, int elen, int i) {
  if (i + 2 + elen <= len) return false;
  if (elen > 200) return true;
  return id == 64 && elen == 128 && phantom_liteon_ahead(ies, len, i);
}

// After a TLV parse failure, slides forward up to kTlvResyncMax bytes to the
// next plausible IE header (an id and length that fit in the buffer).
int tlv_resync(const uint8_t *ies, int len, int start) {
  int end = start + kTlvResyncMax;
  if (end > len - 1) end = len - 1;
  for (int j = start; j < end; j++) {
    const int elen = ies[j + 1];
    if (elen <= 200 && j + 2 + elen <= len) return j;
  }
  return -1;
}

bool sig_append(char *out, size_t cap, size_t *pos, const char *part) {
  const size_t plen = strlen(part);
  if (*pos != 0) {
    if (*pos + 1 >= cap) return false;
    out[(*pos)++] = ',';
  }
  if (*pos + plen >= cap) return false;
  memcpy(out + *pos, part, plen);
  *pos += plen;
  out[*pos] = '\0';
  return true;
}

// Non-vendor IEs are recorded as their decimal tag id.
bool sig_append_tag(char *out, size_t cap, size_t *pos, uint8_t id) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(id));
  return sig_append(out, cap, pos, buf);
}

// Vendor IEs are recorded as "221:" plus up to 8 payload bytes in hex.
bool sig_append_vendor(char *out, size_t cap, size_t *pos, const uint8_t *body, int elen) {
  char buf[24];
  const int take = elen < 8 ? elen : 8;
  memcpy(buf, "221:", 4);
  hex_bytes(buf + 4, body, take);
  buf[4 + take * 2] = '\0';
  return sig_append(out, cap, pos, buf);
}

// Walks the IE TLVs, skipping SSID, encoding vendor payloads and otherwise
// recording tag numbers. Tolerates phantom overflows and resynchronizes after
// corrupt headers. Sets *complete when every byte was consumed.
bool build_signature_from_ies(const uint8_t *ies, int len, char *out, size_t cap,
                              bool *complete) {
  if (!ies || len < 2 || !out || cap < 2) return false;
  size_t pos = 0;
  out[0] = '\0';
  int i = 0;
  int phantom_skips = 0;
  while (i + 2 <= len) {
    const uint8_t id = ies[i];
    const int elen = ies[i + 1];
    if (i + 2 + elen > len) {
      if (phantom_skips < kPhantomSkipCap && is_phantom_overflow(ies, len, id, elen, i)) {
        phantom_skips++;
        i += 2;
        continue;
      }
      const int j = tlv_resync(ies, len, i);
      if (j > i) {
        i = j;
        continue;
      }
      return false;
    }
    i += 2;
    if (id == kIeSsid) {
      if (elen == 0) {
        while (i + 2 <= len && ies[i] == 0 && ies[i + 1] == 0) i += 2;
      } else {
        i += elen;
      }
      continue;
    }
    if (id == kIeVendor && elen >= 4) {
      if (!sig_append_vendor(out, cap, &pos, ies + i, elen)) return false;
    } else {
      if (!sig_append_tag(out, cap, &pos, id)) return false;
    }
    i += elen;
  }
  if (complete) *complete = (i == len);
  return pos > 0;
}

// Normalizes to "2,12,127,<rest from the LiteON anchor>" when the LiteON
// vendor prefix is present but leading tags were lost to parse skew.
void canonicalize_signature(char *sig, size_t cap) {
  if (!sig || cap < 8) return;
  if (strncmp(sig, "2,12,127,", 9) == 0 && strstr(sig, kLiteonIePrefix) != nullptr) return;
  const char *anchor = strstr(sig, kLiteonIePrefix);
  if (!anchor) return;
  char tmp[kSignatureCap];
  const int n = snprintf(tmp, sizeof(tmp), "2,12,127,%s", anchor);
  if (n > 0 && static_cast<size_t>(n) < cap) memcpy(sig, tmp, static_cast<size_t>(n) + 1);
}

void copy_bounded(char *out, size_t cap, const char *src) {
  strncpy(out, src, cap - 1);
  out[cap - 1] = '\0';
}

// Prefers a complete parse, then the longer signature.
bool pick_better_signature(const char *a, bool a_complete, const char *b, bool b_complete,
                           char *out, size_t cap) {
  if (!a[0] && !b[0]) return false;
  const char *pick = a;
  if (!a[0]) {
    pick = b;
  } else if (b[0]) {
    if (!a_complete && b_complete) {
      pick = b;
    } else if (a_complete == b_complete && strlen(b) > strlen(a)) {
      pick = b;
    }
  }
  copy_bounded(out, cap, pick);
  return true;
}

bool signature_is_primary(const char *sig) { return strcmp(sig, kPrimaryIeSignature) == 0; }

// ---------------------------------------------------------------------------
// Flock SoftAP SSID
// ---------------------------------------------------------------------------
//
// The camera firmware's WifiApService names its access point "Flock-" plus
// the last characters of its MAC (firmware dump; also ryanohoro and GainSec
// field research). Matched anchored, case-insensitively, with a tail of four
// or more hex digits and nothing else, so "Flock-Party" never matches.

constexpr uint8_t kSubtypeProbeResponse = 5;
constexpr uint8_t kSubtypeBeacon = 8;
constexpr size_t kBeaconFixedLen = 12;  // timestamp, interval, capability
constexpr char kFlockSsidPrefix[] = "flock-";
constexpr size_t kFlockSsidPrefixLen = sizeof(kFlockSsidPrefix) - 1;
constexpr size_t kMinHexTail = 4;

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_hex(char c) {
  c = ascii_lower(c);
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool starts_with_ignore_case(const char *text, size_t len, const char *prefix_lower, size_t n) {
  if (len < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (ascii_lower(text[i]) != prefix_lower[i]) return false;
  }
  return true;
}

// `prefix_lower` followed by kMinHexTail or more hex digits and nothing else.
bool prefix_hex_tail(const char *text, size_t len, const char *prefix_lower, size_t n) {
  if (len < n + kMinHexTail || !starts_with_ignore_case(text, len, prefix_lower, n)) return false;
  for (size_t i = n; i < len; i++) {
    if (!is_hex(text[i])) return false;
  }
  return true;
}

// The SSID a network announces about itself: the SSID element of a beacon or
// probe response, whose transmitter is the access point. A probe request
// names a network the client is looking for, which says nothing about the
// client, so it gives none.
bool frame_ssid(const uint8_t *frame, size_t len, const uint8_t **ssid, size_t *ssid_len) {
  if (len < kMacHeaderLen) return false;
  const uint8_t fc0 = frame[0];
  if (((fc0 >> 2) & 0x03) != 0) return false;  // not management
  const uint8_t subtype = (fc0 >> 4) & 0x0f;
  if (subtype != kSubtypeBeacon && subtype != kSubtypeProbeResponse) return false;
  const size_t pos = kMacHeaderLen + kBeaconFixedLen;
  if (pos + 2 > len || frame[pos] != kIeSsid) return false;
  const size_t n = frame[pos + 1];
  if (n > 32 || pos + 2 + n > len) return false;
  *ssid = frame + pos + 2;
  *ssid_len = n;
  return true;
}

bool flock_ssid(const uint8_t *ssid, size_t n) {
  return prefix_hex_tail(reinterpret_cast<const char *>(ssid), n, kFlockSsidPrefix,
                         kFlockSsidPrefixLen);
}

// ---------------------------------------------------------------------------
// BLE signatures
// ---------------------------------------------------------------------------
//
// Flock (firmware dump 2026-09-16, ryanohoro, and field captures in
// soyboi1312/all-cameras-are-beacons):
//   name      "Penguin-" + 10 digits or "FS Ext Battery" (battery packs,
//             specific); "FS-" + 4 or more hex digits (a generic white-label
//             prefix, possible); a name starting "Flock" (loose, possible);
//             "DfuTarg" (Nordic's legacy DFU bootloader default, possible)
//   mfg data  company ID 0x09C8, Bluetooth SIG "XUNTONG", the battery
//             packs' module vendor (shared silicon, possible)
//   services  Flock accessory 128-bit UUID (specific), Nordic legacy DFU
//             128-bit UUID (possible), Raven 16-bit UUIDs 0x3100, 0x3200,
//             0x3300, 0x3400, and 0x3500 (field capture; not SIG-assigned;
//             specific). Raven advertises them in 128-bit form on the
//             Bluetooth base UUID, so both forms are read.
// Axon (all-cameras-are-beacons field captures; Bluetooth SIG):
//   "BWCDEVICE" inside service UUID or service-data bytes, in either byte
//   order (on air it reads "AXJANUSBWCDEVICE" reversed); company ID 0x034D
//   and member UUIDs 0xFE6B and 0xFE6C ("TASER International, Inc."), and
//   0xFC81 ("Axon Enterprise, Inc."). All specific.
// Utility Inc. BodyWorn: activation remote named "BodyWorn Remote"
//   (nite-oui-collection capture). Specific.
// Motorola Solutions (Bluetooth SIG): company ID 0x04EC and member UUIDs
//   0xFD8E and 0xFE04. Mostly MOTOTRBO radios carried by retail, school, and
//   venue staff as well as police, so possible, and switchable.
// Retired: a bare 10-digit name (it matched a phone named "0102000000" in
// the field). Generic names such as "msm8953_32" or "Android" are not matched.

constexpr uint16_t kCompanyXuntong = 0x09c8;
constexpr uint16_t kCompanyAxon = 0x034d;
constexpr uint16_t kCompanyMotorola = 0x04ec;
constexpr uint16_t kRavenServices[] = {0x3100, 0x3200, 0x3300, 0x3400, 0x3500};
constexpr uint16_t kAxonServices[] = {0xfe6b, 0xfe6c, 0xfc81};
constexpr uint16_t kMotorolaServices[] = {0xfd8e, 0xfe04};
constexpr char kAxonTag[] = "BWCDEVICE";
constexpr size_t kAxonTagLen = sizeof(kAxonTag) - 1;

// 128-bit UUIDs in canonical (big-endian, as written) byte order. Advertising
// data carries them little-endian.
constexpr uint8_t kFlockServiceUuid[16] = {0xe8, 0xcc, 0xbb, 0x38, 0x95, 0x32, 0x46, 0xa8,
                                           0x9f, 0xe5, 0x18, 0x14, 0xdf, 0x17, 0x2e, 0x6f};
constexpr uint8_t kNordicDfuServiceUuid[16] = {0x00, 0x00, 0x15, 0x30, 0x12, 0x12, 0xef, 0xde,
                                               0x15, 0x23, 0x78, 0x5f, 0xea, 0xbc, 0xd1, 0x23};
// Bluetooth base UUID 0000xxxx-0000-1000-8000-00805F9B34FB as it appears on
// air (little-endian), bytes 0-11; bytes 12-13 carry the 16-bit short.
constexpr uint8_t kBaseUuidLow[12] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00,
                                      0x00, 0x80, 0x00, 0x10, 0x00, 0x00};

constexpr uint8_t kAdNameShort = 0x08;
constexpr uint8_t kAdNameComplete = 0x09;
constexpr uint8_t kAdUuid16Incomplete = 0x02;
constexpr uint8_t kAdUuid16Complete = 0x03;
constexpr uint8_t kAdUuid128Incomplete = 0x06;
constexpr uint8_t kAdUuid128Complete = 0x07;
constexpr uint8_t kAdServiceData16 = 0x16;
constexpr uint8_t kAdServiceData32 = 0x20;
constexpr uint8_t kAdServiceData128 = 0x21;
constexpr uint8_t kAdManufacturerData = 0xff;

bool uuid128_le_equals(const uint8_t *le, const uint8_t *canonical) {
  for (int i = 0; i < 16; i++) {
    if (le[i] != canonical[15 - i]) return false;
  }
  return true;
}

bool equals_ignore_case(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    if (ascii_lower(*a) != ascii_lower(*b)) return false;
  }
  return *a == *b;
}

bool is_digits(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

bool name_is_penguin_serial(const char *n) {
  return starts_with_ignore_case(n, strlen(n), "penguin-", 8) && is_digits(n + 8, 10) &&
         n[18] == '\0';
}

template <size_t N>
bool listed(const uint16_t (&list)[N], uint16_t value) {
  for (uint16_t v : list) {
    if (v == value) return true;
  }
  return false;
}

// True when `tag` appears in the bytes, forwards or reversed.
bool contains_tag(const uint8_t *bytes, size_t len) {
  if (len < kAxonTagLen) return false;
  for (size_t i = 0; i + kAxonTagLen <= len; i++) {
    bool forward = true, reversed = true;
    for (size_t k = 0; k < kAxonTagLen && (forward || reversed); k++) {
      const char c = static_cast<char>(bytes[i + k]);
      forward = forward && c == kAxonTag[k];
      reversed = reversed && c == kAxonTag[kAxonTagLen - 1 - k];
    }
    if (forward || reversed) return true;
  }
  return false;
}

// Candidate rules found in one advert, recorded so the strongest can be
// chosen after the whole advert is parsed.
struct BleFindings {
  bool has_name = false;
  bool name_complete = false;
  char name[33] = {};
  bool mfg_xuntong = false;
  bool mfg_axon = false;
  bool mfg_motorola = false;
  bool svc_flock = false;
  bool svc_nordic_dfu = false;
  bool svc_raven = false;
  bool svc_axon = false;
  bool svc_motorola = false;
  bool axon_tag = false;
};

void note_uuid16(BleFindings *f, uint16_t u) {
  if (listed(kRavenServices, u)) f->svc_raven = true;
  if (listed(kAxonServices, u)) f->svc_axon = true;
  if (listed(kMotorolaServices, u)) f->svc_motorola = true;
}

// A 128-bit UUID (little-endian): Flock or Nordic services, or a 16-bit
// UUID written in base-UUID form.
void note_uuid128(BleFindings *f, const uint8_t *le) {
  if (uuid128_le_equals(le, kFlockServiceUuid)) f->svc_flock = true;
  if (uuid128_le_equals(le, kNordicDfuServiceUuid)) f->svc_nordic_dfu = true;
  if (memcmp(le, kBaseUuidLow, sizeof(kBaseUuidLow)) == 0 && le[14] == 0 && le[15] == 0) {
    note_uuid16(f, static_cast<uint16_t>(le[12] | (le[13] << 8)));
  }
}

void parse_ad(const uint8_t *ad, size_t ad_len, BleFindings *f) {
  size_t pos = 0;
  while (pos + 1 < ad_len) {
    const size_t field_len = ad[pos];
    if (field_len == 0) break;  // early termination / padding
    if (pos + 1 + field_len > ad_len) break;  // malformed: overruns the buffer
    const uint8_t type = ad[pos + 1];
    const uint8_t *data = ad + pos + 2;
    const size_t data_len = field_len - 1;

    switch (type) {
      case kAdNameShort:
      case kAdNameComplete:
        // A complete name replaces a shortened one, never the reverse.
        if (!f->has_name || (type == kAdNameComplete && !f->name_complete)) {
          size_t n = data_len < sizeof(f->name) - 1 ? data_len : sizeof(f->name) - 1;
          memcpy(f->name, data, n);
          f->name[n] = '\0';  // an embedded NUL also ends the name
          f->has_name = true;
          f->name_complete = (type == kAdNameComplete);
        }
        break;
      case kAdManufacturerData:
        if (data_len >= 2) {
          const uint16_t cid = static_cast<uint16_t>(data[0] | (data[1] << 8));
          if (cid == kCompanyXuntong) f->mfg_xuntong = true;
          if (cid == kCompanyAxon) f->mfg_axon = true;
          if (cid == kCompanyMotorola) f->mfg_motorola = true;
        }
        break;
      case kAdUuid16Incomplete:
      case kAdUuid16Complete:
        for (size_t i = 0; i + 2 <= data_len; i += 2) {
          note_uuid16(f, static_cast<uint16_t>(data[i] | (data[i + 1] << 8)));
        }
        break;
      case kAdUuid128Incomplete:
      case kAdUuid128Complete:
        for (size_t i = 0; i + 16 <= data_len; i += 16) note_uuid128(f, data + i);
        if (contains_tag(data, data_len)) f->axon_tag = true;
        break;
      case kAdServiceData16:
        if (data_len >= 2) note_uuid16(f, static_cast<uint16_t>(data[0] | (data[1] << 8)));
        if (contains_tag(data, data_len)) f->axon_tag = true;
        break;
      case kAdServiceData32:
        if (contains_tag(data, data_len)) f->axon_tag = true;
        break;
      case kAdServiceData128:
        if (data_len >= 16) note_uuid128(f, data);
        if (contains_tag(data, data_len)) f->axon_tag = true;
        break;
      default:
        break;
    }
    pos += 1 + field_len;
  }
}

Rule name_rule(const char *name) {
  const size_t len = strlen(name);
  if (name_is_penguin_serial(name)) return Rule::BleNamePenguinSerial;
  if (equals_ignore_case(name, "fs ext battery")) return Rule::BleNameExtBattery;
  if (starts_with_ignore_case(name, len, "bodyworn remote", 15)) return Rule::BleNameBodyWorn;
  if (equals_ignore_case(name, "dfutarg")) return Rule::BleNameDfuTarg;
  if (prefix_hex_tail(name, len, "fs-", 3)) return Rule::BleNameFsHex;
  if (starts_with_ignore_case(name, len, "flock", 5)) return Rule::BleNameFlock;
  return Rule::Count;
}

}  // namespace

const RuleInfo &rule_info(Rule rule) {
  const size_t i = static_cast<size_t>(rule);
  return kRuleInfo[i < static_cast<size_t>(Rule::Count) ? i : 0];
}

namespace {

constexpr DeviceTypeInfo kDeviceTypeInfo[] = {
    {"Flock camera", "Flock camera", false},
    {"Flock?", "Flock (possible)", true},
    {"Penguin bat", "Penguin battery", false},
    {"Penguin bat?", "Penguin battery (possible)", true},
    {"Flock accsry", "Flock accessory", false},
    {"Flock Raven", "Flock Raven", false},
    {"Nordic DFU?", "Nordic DFU (possible)", true},
    {"ALPR", "ALPR equipment", false},
    {"ALPR?", "ALPR equipment (possible)", true},
    {"Body camera", "Body camera", false},
    {"Body camera?", "Body camera (possible)", true},
    {"Radio", "Two-way radio", false},
    {"Radio?", "Two-way radio (possible)", true},
    {"Vehicle net?", "Vehicle network (possible)", true},
    {"Speed radar", "Speed radar", false},
    {"Light bar", "Light bar", false},
    {"Drone?", "Drone (possible)", true},
    {"Gunshot sens", "Gunshot sensor", false},
};
static_assert(sizeof(kDeviceTypeInfo) / sizeof(kDeviceTypeInfo[0]) ==
                  static_cast<size_t>(DeviceType::Count),
              "every device type needs metadata");

}  // namespace

const DeviceTypeInfo &device_type_info(DeviceType type) {
  const size_t i = static_cast<size_t>(type);
  return kDeviceTypeInfo[i < static_cast<size_t>(DeviceType::Count) ? i : 0];
}

const OuiEntry *watchlist_lookup(const uint8_t mac[6]) { return find_watchlist(mac); }

const char *flock_radio_oui_vendor(const uint8_t mac[6]) {
  const RadioOui *r = find_radio_oui(mac);
  return r ? r->vendor : nullptr;
}

void set_options(const Options &options) { g_options = options; }
const Options &options() { return g_options; }

Classification classify(Rule rule, const uint8_t mac[6]) {
  switch (rule) {
    case Rule::WifiOuiTransmitter:
    case Rule::WifiOuiReceiver:
    case Rule::WifiOuiBssid:
    case Rule::BleOui: {
      if (const OuiEntry *e = find_watchlist(mac)) {
        return {type_for(e->category, e->strength), e->strength, e->vendor};
      }
      // Radio-module OUIs (matched on any frame only when enabled), and
      // prefixes earlier builds matched.
      return {DeviceType::FlockPossible, Strength::Possible, flock_radio_oui_vendor(mac)};
    }
    case Rule::WifiProbeIeSignature:
      return {DeviceType::FlockCamera, Strength::Specific, flock_radio_oui_vendor(mac)};
    case Rule::WifiWildcardProbe:
      return {DeviceType::FlockPossible, Strength::Possible, flock_radio_oui_vendor(mac)};
    case Rule::WifiSsidFlock:
      return {DeviceType::FlockCamera, Strength::Specific, "Flock Safety"};
    case Rule::BleNamePenguinSerial:
    case Rule::BleNameExtBattery:
      return {DeviceType::PenguinBattery, Strength::Specific, "Flock Safety"};
    case Rule::BleNameBareSerial:
    case Rule::BleNameFsHex:
      return {DeviceType::PenguinBatteryPossible, Strength::Possible, nullptr};
    case Rule::BleMfgXuntong:
      return {DeviceType::PenguinBatteryPossible, Strength::Possible, "XUNTONG"};
    case Rule::BleServiceFlock:
      return {DeviceType::FlockAccessory, Strength::Specific, "Flock Safety"};
    case Rule::BleServiceRaven:
      return {DeviceType::FlockRaven, Strength::Specific, "Flock Safety"};
    case Rule::BleAxonTag:
    case Rule::BleMfgAxon:
    case Rule::BleServiceAxon:
      return {DeviceType::BodyCamera, Strength::Specific, "Axon"};
    case Rule::BleNameBodyWorn:
      return {DeviceType::BodyCamera, Strength::Specific, "Utility Inc. (BodyWorn)"};
    case Rule::BleMfgMotorola:
    case Rule::BleServiceMotorola:
      return {DeviceType::RadioPossible, Strength::Possible, "Motorola Solutions"};
    case Rule::BleNameFlock:
      return {DeviceType::FlockPossible, Strength::Possible, nullptr};
    case Rule::BleNameDfuTarg:
    case Rule::BleServiceNordicDfu:
    default:
      return {DeviceType::NordicDfuPossible, Strength::Possible, nullptr};
  }
}

size_t flock_radio_oui_count() { return kFlockRadioOuiCount; }
size_t watchlist_count() { return kWatchlistCount; }

bool wifi_frame_is_candidate(const uint8_t *frame, size_t len, bool is_mgmt) {
  if (!frame || len < kMacHeaderLen) return false;
  const uint8_t *addr1 = frame + kAddr1Offset;
  const uint8_t *addr2 = frame + kAddr2Offset;
  const bool any_frame = g_options.radio_ouis_any_frame;
  if (watched(addr2)) return true;
  if (find_radio_oui(addr2) && (any_frame || (is_mgmt && is_probe_request(frame)))) return true;
  if (!is_multicast(addr1) && (watched(addr1) || (any_frame && find_radio_oui(addr1)))) {
    return true;
  }
  if (!is_mgmt) return false;
  const uint8_t *addr3 = frame + kAddr3Offset;
  if (watched(addr3) || (any_frame && find_radio_oui(addr3))) return true;
  const uint8_t *ssid;
  size_t ssid_len;
  return frame_ssid(frame, len, &ssid, &ssid_len) && flock_ssid(ssid, ssid_len);
}

bool build_probe_ie_signature(const uint8_t *body, size_t len, char *out, size_t cap) {
  if (!body || len < 2 || !out || cap < 16 || len > 4096) return false;
  const int body_len = static_cast<int>(len);
  char sig_a[kSignatureCap] = {};
  char sig_b[kSignatureCap] = {};
  bool complete_a = false;
  bool complete_b = false;
  const bool ok_a = build_signature_from_ies(body, body_len, sig_a, sizeof(sig_a), &complete_a);
  bool ok_b = false;
  // Also try without a leading empty SSID IE pair.
  if (body[0] == 0 && body[1] == 0) {
    ok_b = build_signature_from_ies(body + 2, body_len - 2, sig_b, sizeof(sig_b), &complete_b);
  }
  char merged[kSignatureCap] = {};
  if (!pick_better_signature(ok_a ? sig_a : "", complete_a, ok_b ? sig_b : "", complete_b,
                             merged, sizeof(merged))) {
    return false;
  }
  canonicalize_signature(merged, sizeof(merged));
  copy_bounded(out, cap, merged);
  return out[0] != '\0';
}

bool probe_body_has_primary_signature(const uint8_t *body, size_t len) {
  char sig[kSignatureCap];
  if (build_probe_ie_signature(body, len, sig, sizeof(sig)) && signature_is_primary(sig)) {
    return true;
  }
  return len > kFcsLen && build_probe_ie_signature(body, len - kFcsLen, sig, sizeof(sig)) &&
         signature_is_primary(sig);
}

size_t match_wifi_frame(const uint8_t *frame, size_t len, bool is_mgmt, Match *out,
                        size_t out_cap) {
  if (!frame || len < kMacHeaderLen || !out || out_cap == 0) return 0;
  size_t count = 0;
  auto push = [&](Rule rule, Strength strength, const uint8_t *mac) -> Match * {
    if (count >= out_cap) return nullptr;
    Match &m = out[count++];
    m.rule = rule;
    m.strength = strength;
    memcpy(m.mac, mac, 6);
    m.name[0] = '\0';
    return &m;
  };
  const bool any_frame = g_options.radio_ouis_any_frame;

  const uint8_t *addr1 = frame + kAddr1Offset;
  const uint8_t *addr2 = frame + kAddr2Offset;
  const uint8_t *addr3 = frame + kAddr3Offset;

  // Transmitter (addr2). A wildcard probe request from a Flock radio OUI is
  // checked against the IE fingerprint; otherwise a watchlist OUI is a
  // transmitter match at its vendor's strength. One frame never yields two
  // addr2 OUI matches.
  const OuiEntry *w2 = watched(addr2);
  const bool radio2 = find_radio_oui(addr2) != nullptr;
  bool addr2_matched = false;
  if (radio2 && is_mgmt && is_probe_request(frame)) {
    const uint8_t *body = frame + kMacHeaderLen;
    const int body_len = static_cast<int>(len - kMacHeaderLen);
    int state = body_len > 0 ? wildcard_ssid_state(body, body_len) : -1;
    // Retry without the FCS only when no SSID IE was found at all; a
    // non-empty SSID means a directed probe and must not be reclassified.
    if (state == -1 && body_len > static_cast<int>(kFcsLen)) {
      state = wildcard_ssid_state(body, body_len - static_cast<int>(kFcsLen));
    }
    if (state == 1) {
      if (probe_body_has_primary_signature(body, len - kMacHeaderLen)) {
        push(Rule::WifiProbeIeSignature, Strength::Specific, addr2);
        addr2_matched = true;
      } else if (!w2) {
        push(Rule::WifiWildcardProbe, Strength::Possible, addr2);
        addr2_matched = true;
      }
    }
  }
  if (!addr2_matched) {
    if (w2) {
      push(Rule::WifiOuiTransmitter, w2->strength, addr2);
    } else if (radio2 && any_frame) {
      push(Rule::WifiOuiTransmitter, Strength::Possible, addr2);
    }
  }

  // Receiver (addr1): an AP answering a device's probe puts the device's MAC
  // in addr1. Second-hand and noisier, but it surfaces stations that stay
  // silent through the whole dwell window.
  if (!is_multicast(addr1)) {
    if (const OuiEntry *w1 = watched(addr1)) {
      push(Rule::WifiOuiReceiver, w1->strength, addr1);
    } else if (any_frame && find_radio_oui(addr1)) {
      push(Rule::WifiOuiReceiver, Strength::Possible, addr1);
    }
  }

  // BSSID (addr3) of management frames: catches an OUI when addr2 is
  // randomized.
  if (is_mgmt) {
    if (const OuiEntry *w3 = watched(addr3)) {
      push(Rule::WifiOuiBssid, w3->strength, addr3);
    } else if (any_frame && find_radio_oui(addr3)) {
      push(Rule::WifiOuiBssid, Strength::Possible, addr3);
    }
  }

  // A Flock SoftAP name announced by the access point itself.
  const uint8_t *ssid;
  size_t ssid_len;
  if (is_mgmt && frame_ssid(frame, len, &ssid, &ssid_len) && flock_ssid(ssid, ssid_len)) {
    if (Match *m = push(Rule::WifiSsidFlock, Strength::Specific, addr2)) {
      memcpy(m->name, ssid, ssid_len);
      m->name[ssid_len] = '\0';
    }
  }
  return count;
}

bool match_ble_advert(const uint8_t addr[6], bool public_addr, const uint8_t *ad, size_t ad_len,
                      Match *out) {
  if (!addr || !ad || !out) return false;
  BleFindings f;
  parse_ad(ad, ad_len, &f);
  const bool motorola = g_options.motorola;
  const OuiEntry *oui = public_addr ? watched(addr) : nullptr;

  struct Candidate {
    Rule rule;
    Strength strength;
  };
  auto rule_strength = [](Rule r) { return Candidate{r, rule_info(r).strength}; };
  // In priority order within a strength: name, payload tag, services,
  // manufacturer data, then the address OUI.
  const Candidate candidates[] = {
      rule_strength(f.has_name ? name_rule(f.name) : Rule::Count),
      rule_strength(f.axon_tag ? Rule::BleAxonTag : Rule::Count),
      rule_strength(f.svc_flock ? Rule::BleServiceFlock : Rule::Count),
      rule_strength(f.svc_raven ? Rule::BleServiceRaven : Rule::Count),
      rule_strength(f.svc_axon ? Rule::BleServiceAxon : Rule::Count),
      rule_strength(f.mfg_axon ? Rule::BleMfgAxon : Rule::Count),
      rule_strength(f.svc_nordic_dfu ? Rule::BleServiceNordicDfu : Rule::Count),
      rule_strength(motorola && f.svc_motorola ? Rule::BleServiceMotorola : Rule::Count),
      rule_strength(motorola && f.mfg_motorola ? Rule::BleMfgMotorola : Rule::Count),
      rule_strength(f.mfg_xuntong ? Rule::BleMfgXuntong : Rule::Count),
      oui ? Candidate{Rule::BleOui, oui->strength} : Candidate{Rule::Count, Strength::Possible},
  };

  const Candidate *chosen = nullptr;
  for (Strength want : {Strength::Specific, Strength::Possible}) {
    for (const Candidate &c : candidates) {
      if (c.rule != Rule::Count && c.strength == want) {
        chosen = &c;
        break;
      }
    }
    if (chosen) break;
  }
  if (!chosen) return false;

  out->rule = chosen->rule;
  out->strength = chosen->strength;
  memcpy(out->mac, addr, 6);
  memcpy(out->name, f.name, sizeof(out->name));
  return true;
}

}  // namespace fy
