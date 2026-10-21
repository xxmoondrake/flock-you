// SPDX-License-Identifier: MIT
//
// Signature data and matching logic ported from the upstream flock-you
// firmware (main.cpp at commit 6704f01), Copyright (c) 2026 colonelpanichacks,
// MIT License; see LICENSE in the repository root. Provenance notes for each
// signature are kept from upstream.
//
// Differences from upstream, by design:
// - Matching runs in a task, not in the radio callbacks. The Wi-Fi callback
//   only calls wifi_frame_is_candidate().
// - Each rule carries a strength (specific or possible) in addition to the
//   upstream tier.
// - BLE adverts are parsed from raw AD structures instead of NimBLE-Arduino
//   objects, and a specific BLE rule wins over a possible one.
// - The SSID keyword path (disabled upstream) is not ported.

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
// signature (alertTypeToMethod and alertTypeToTier in upstream main.cpp).
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
};
static_assert(sizeof(kRuleInfo) / sizeof(kRuleInfo[0]) == static_cast<size_t>(Rule::Count),
              "every rule needs metadata");

// ---------------------------------------------------------------------------
// Target OUIs
// ---------------------------------------------------------------------------
//
// Union of two provenance sets (34 prefixes), unchanged from upstream:
//
// COMMUNITY FIELD RESEARCH (@NitekryDPaul / DeFlockJoplin), 32 prefixes,
// synced upstream with nite-oui-collection my_tested_flock.md (2026-07-16):
// 31 active prefixes plus 82:6b:f2 from DeFlockJoplin. 82:6b:f2 has the
// locally administered bit set, so matching must not filter locally
// administered addresses.
//
// FIRMWARE EXTRACTED (Flock Safety camera firmware dump, 2026-09-16; Qualcomm
// MSM8953 + QCA9377 radio):
//   b4:1e:52  Flock Safety's IEEE-registered OUI
//   00:03:7f  Qualcomm Atheros; the QCA9377 firmware default MACs use it
//
// Every OUI rule is a possible match: a prefix identifies a radio vendor, not
// a camera. Individual prefixes are still to be reviewed against the parity
// reference (see Planned in the firmware README).
constexpr uint8_t kTargetOuis[][3] = {
    {0x70, 0xc9, 0x4e}, {0x3c, 0x91, 0x80}, {0xd8, 0xf3, 0xbc}, {0x80, 0x30, 0x49},
    {0xb8, 0x35, 0x32}, {0x14, 0x5a, 0xfc}, {0x74, 0x4c, 0xa1}, {0x08, 0x3a, 0x88},
    {0x9c, 0x2f, 0x9d}, {0xc0, 0x35, 0x32}, {0x94, 0x08, 0x53}, {0xe4, 0xaa, 0xea},
    {0xf4, 0x6a, 0xdd}, {0xe0, 0x0a, 0xf6}, {0x24, 0xb2, 0xb9}, {0x00, 0xf4, 0x8d},
    {0xd0, 0x39, 0x57}, {0xe8, 0xd0, 0xfc}, {0xe0, 0x4f, 0x43}, {0xb8, 0x1e, 0xa4},
    {0x70, 0x08, 0x94}, {0x58, 0x8e, 0x81}, {0xec, 0x1b, 0xbd}, {0x3c, 0x71, 0xbf},
    {0x58, 0x00, 0xe3}, {0x90, 0x35, 0xea}, {0x5c, 0x93, 0xa2}, {0x64, 0x6e, 0x69},
    {0x48, 0x27, 0xea}, {0xa4, 0xcf, 0x12}, {0x14, 0xb5, 0xcd},
    {0x82, 0x6b, 0xf2},  // DeFlockJoplin (locally administered bit set)
    {0xb4, 0x1e, 0x52},  // Flock Safety (IEEE MA-L)
    {0x00, 0x03, 0x7f},  // Qualcomm Atheros QCA9377 firmware default MACs
};
constexpr size_t kTargetOuiCount = sizeof(kTargetOuis) / sizeof(kTargetOuis[0]);

// No locally administered pre-filter: 82:6b:f2 has that bit set. Randomized
// phone MACs simply fail the comparison.
bool oui_matches(const uint8_t *mac) {
  for (size_t i = 0; i < kTargetOuiCount; i++) {
    if (mac[0] == kTargetOuis[i][0] && mac[1] == kTargetOuis[i][1] &&
        mac[2] == kTargetOuis[i][2]) {
      return true;
    }
  }
  return false;
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
// BLE signatures (firmware dump, 2026-09-16)
// ---------------------------------------------------------------------------
//
// Penguin battery packs and Raven cameras advertise over BLE:
//   name      "Penguin-" + 10 digits, a bare 10-digit serial, "FS Ext
//             Battery", or "DfuTarg" (a Penguin battery receiving a Nordic
//             legacy DFU update)
//   mfg data  company ID 0x09C8 (XUNTONG, Penguin battery)
//   services  Flock accessory 128-bit UUID, Nordic legacy DFU 128-bit UUID,
//             or a 16-bit UUID in 0x3100-0x3500 (Raven GATT services)
// Generic names such as "msm8953_32" or "Android" are deliberately not matched.
//
// Strength assignments: "DfuTarg", the Nordic DFU service, a bare 10-digit
// name, and a company ID are shared by unrelated products, so they are
// possible matches. The Raven range is not SIG-assigned and is treated as
// specific pending review against the parity reference.

constexpr uint16_t kCompanyXuntong = 0x09c8;
constexpr uint16_t kRavenServiceMin = 0x3100;
constexpr uint16_t kRavenServiceMax = 0x3500;

// 128-bit UUIDs in canonical (big-endian, as written) byte order. Advertising
// data carries them little-endian.
constexpr uint8_t kFlockServiceUuid[16] = {0xe8, 0xcc, 0xbb, 0x38, 0x95, 0x32, 0x46, 0xa8,
                                           0x9f, 0xe5, 0x18, 0x14, 0xdf, 0x17, 0x2e, 0x6f};
constexpr uint8_t kNordicDfuServiceUuid[16] = {0x00, 0x00, 0x15, 0x30, 0x12, 0x12, 0xef, 0xde,
                                               0x15, 0x23, 0x78, 0x5f, 0xea, 0xbc, 0xd1, 0x23};

constexpr uint8_t kAdNameShort = 0x08;
constexpr uint8_t kAdNameComplete = 0x09;
constexpr uint8_t kAdUuid16Incomplete = 0x02;
constexpr uint8_t kAdUuid16Complete = 0x03;
constexpr uint8_t kAdUuid128Incomplete = 0x06;
constexpr uint8_t kAdUuid128Complete = 0x07;
constexpr uint8_t kAdManufacturerData = 0xff;

bool uuid128_le_equals(const uint8_t *le, const uint8_t *canonical) {
  for (int i = 0; i < 16; i++) {
    if (le[i] != canonical[15 - i]) return false;
  }
  return true;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

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
  static const char kPrefix[] = "penguin-";
  for (size_t i = 0; i < 8; i++) {
    if (ascii_lower(n[i]) != kPrefix[i]) return false;
  }
  return is_digits(n + 8, 10) && n[18] == '\0';
}

bool name_is_bare_serial(const char *n) { return is_digits(n, 10) && n[10] == '\0'; }

// Candidate rules found in one advert, recorded so the strongest can be
// chosen after the whole advert is parsed.
struct BleFindings {
  bool has_name = false;
  bool name_complete = false;
  char name[33] = {};
  bool mfg_xuntong = false;
  bool svc_flock = false;
  bool svc_nordic_dfu = false;
  bool svc_raven = false;
};

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
        }
        break;
      case kAdUuid16Incomplete:
      case kAdUuid16Complete:
        for (size_t i = 0; i + 2 <= data_len; i += 2) {
          const uint16_t u = static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
          if (u >= kRavenServiceMin && u <= kRavenServiceMax) f->svc_raven = true;
        }
        break;
      case kAdUuid128Incomplete:
      case kAdUuid128Complete:
        for (size_t i = 0; i + 16 <= data_len; i += 16) {
          if (uuid128_le_equals(data + i, kFlockServiceUuid)) f->svc_flock = true;
          if (uuid128_le_equals(data + i, kNordicDfuServiceUuid)) f->svc_nordic_dfu = true;
        }
        break;
      default:
        break;
    }
    pos += 1 + field_len;
  }
}

}  // namespace

const RuleInfo &rule_info(Rule rule) {
  const size_t i = static_cast<size_t>(rule);
  return kRuleInfo[i < static_cast<size_t>(Rule::Count) ? i : 0];
}

namespace {

constexpr DeviceTypeInfo kDeviceTypeInfo[] = {
    {"Flock camera", false}, {"Flock OUI?", true},     {"Penguin bat", false},
    {"Penguin bat?", true},  {"Flock accsry", false}, {"Flock Raven", false},
    {"Nordic DFU?", true},
};
static_assert(sizeof(kDeviceTypeInfo) / sizeof(kDeviceTypeInfo[0]) ==
                  static_cast<size_t>(DeviceType::Count),
              "every device type needs metadata");

}  // namespace

DeviceType device_type(Rule rule) {
  switch (rule) {
    case Rule::WifiProbeIeSignature:
      return DeviceType::FlockCamera;
    case Rule::WifiWildcardProbe:
    case Rule::WifiOuiTransmitter:
    case Rule::WifiOuiReceiver:
    case Rule::WifiOuiBssid:
      return DeviceType::FlockOuiPossible;
    case Rule::BleNamePenguinSerial:
    case Rule::BleNameExtBattery:
      return DeviceType::PenguinBattery;
    case Rule::BleNameBareSerial:
    case Rule::BleMfgXuntong:
      return DeviceType::PenguinBatteryPossible;
    case Rule::BleServiceFlock:
      return DeviceType::FlockAccessory;
    case Rule::BleServiceRaven:
      return DeviceType::FlockRaven;
    case Rule::BleNameDfuTarg:
    case Rule::BleServiceNordicDfu:
    default:
      return DeviceType::NordicDfuPossible;
  }
}

const DeviceTypeInfo &device_type_info(DeviceType type) {
  const size_t i = static_cast<size_t>(type);
  return kDeviceTypeInfo[i < static_cast<size_t>(DeviceType::Count) ? i : 0];
}

size_t target_oui_count() { return kTargetOuiCount; }

bool wifi_frame_is_candidate(const uint8_t *frame, size_t len, bool is_mgmt) {
  if (!frame || len < kMacHeaderLen) return false;
  const uint8_t *addr1 = frame + kAddr1Offset;
  if (oui_matches(frame + kAddr2Offset)) return true;
  if (!is_multicast(addr1) && oui_matches(addr1)) return true;
  return is_mgmt && oui_matches(frame + kAddr3Offset);
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
  auto push = [&](Rule rule, const uint8_t *mac) {
    if (count >= out_cap) return;
    Match &m = out[count++];
    m.rule = rule;
    memcpy(m.mac, mac, 6);
    m.name[0] = '\0';
  };

  const uint8_t *addr1 = frame + kAddr1Offset;
  const uint8_t *addr2 = frame + kAddr2Offset;
  const uint8_t *addr3 = frame + kAddr3Offset;

  // Transmitter (addr2). A wildcard probe request from a target OUI is
  // checked against the IE fingerprint; any other frame from a target OUI is
  // a broad transmitter match. One frame never yields two addr2 matches.
  if (oui_matches(addr2)) {
    bool probe_matched = false;
    if (is_mgmt && is_probe_request(frame)) {
      const uint8_t *body = frame + kMacHeaderLen;
      const int body_len = static_cast<int>(len - kMacHeaderLen);
      int state = body_len > 0 ? wildcard_ssid_state(body, body_len) : -1;
      // Retry without the FCS only when no SSID IE was found at all; a
      // non-empty SSID means a directed probe and must not be reclassified.
      if (state == -1 && body_len > static_cast<int>(kFcsLen)) {
        state = wildcard_ssid_state(body, body_len - static_cast<int>(kFcsLen));
      }
      if (state == 1) {
        const bool primary = probe_body_has_primary_signature(body, len - kMacHeaderLen);
        push(primary ? Rule::WifiProbeIeSignature : Rule::WifiWildcardProbe, addr2);
        probe_matched = true;
      }
    }
    if (!probe_matched) push(Rule::WifiOuiTransmitter, addr2);
  }

  // Receiver (addr1): an AP answering a camera's probe puts the camera MAC in
  // addr1. Second-hand and noisier, but it surfaces stations that stay silent
  // through the whole dwell window.
  if (!is_multicast(addr1) && oui_matches(addr1)) push(Rule::WifiOuiReceiver, addr1);

  // BSSID (addr3) of management frames: catches a target OUI when addr2 is
  // randomized.
  if (is_mgmt && oui_matches(addr3)) push(Rule::WifiOuiBssid, addr3);

  return count;
}

bool match_ble_advert(const uint8_t addr[6], const uint8_t *ad, size_t ad_len, Match *out) {
  if (!addr || !ad || !out) return false;
  BleFindings f;
  parse_ad(ad, ad_len, &f);

  Rule name_rule = Rule::Count;
  if (f.has_name) {
    if (name_is_penguin_serial(f.name)) {
      name_rule = Rule::BleNamePenguinSerial;
    } else if (equals_ignore_case(f.name, "fs ext battery")) {
      name_rule = Rule::BleNameExtBattery;
    } else if (name_is_bare_serial(f.name)) {
      name_rule = Rule::BleNameBareSerial;
    } else if (equals_ignore_case(f.name, "dfutarg")) {
      name_rule = Rule::BleNameDfuTarg;
    }
  }

  // Upstream order: name, manufacturer data, then services.
  const Rule ordered[] = {
      name_rule,
      f.mfg_xuntong ? Rule::BleMfgXuntong : Rule::Count,
      f.svc_flock ? Rule::BleServiceFlock : Rule::Count,
      f.svc_nordic_dfu ? Rule::BleServiceNordicDfu : Rule::Count,
      f.svc_raven ? Rule::BleServiceRaven : Rule::Count,
  };

  Rule chosen = Rule::Count;
  for (Strength want : {Strength::Specific, Strength::Possible}) {
    for (Rule r : ordered) {
      if (r != Rule::Count && rule_info(r).strength == want) {
        chosen = r;
        break;
      }
    }
    if (chosen != Rule::Count) break;
  }
  if (chosen == Rule::Count) return false;

  out->rule = chosen;
  memcpy(out->mac, addr, 6);
  memcpy(out->name, f.name, sizeof(out->name));
  return true;
}

}  // namespace fy
