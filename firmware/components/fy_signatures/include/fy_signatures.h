// SPDX-License-Identifier: MIT
//
// Wi-Fi and BLE signature matching. Pure C++ with no ESP-IDF dependencies so
// it can be unit-tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy {

// The rule that produced a match. Values are stable identifiers for logs and
// stored records; append new rules before Count.
enum class Rule : uint8_t {
  WifiProbeIeSignature = 0,  // OUI + wildcard probe + drive-tested IE fingerprint
  WifiWildcardProbe,         // OUI + wildcard probe, fingerprint did not match
  WifiOuiTransmitter,        // OUI in addr2 (transmitter), any frame
  WifiOuiReceiver,           // OUI in addr1 (receiver), e.g. an AP's probe response
  WifiOuiBssid,              // OUI in addr3 (BSSID) of a management frame
  BleNamePenguinSerial,      // "Penguin-" followed by exactly 10 digits
  BleNameExtBattery,         // "FS Ext Battery"
  BleNameBareSerial,         // exactly 10 digits
  BleNameDfuTarg,            // "DfuTarg", the Nordic legacy DFU bootloader default
  BleMfgXuntong,             // manufacturer data with company ID 0x09C8
  BleServiceFlock,           // Flock accessory 128-bit service UUID
  BleServiceNordicDfu,       // Nordic legacy DFU 128-bit service UUID
  BleServiceRaven,           // 16-bit service UUID in 0x3100-0x3500
  Count
};

// A specific signature may raise a prominent alert. A possible match rests on
// a broad manufacturer prefix or a generic identifier: it is logged and shown,
// but never presented as confirmed. Neither proves what a device is.
enum class Strength : uint8_t { Possible = 0, Specific = 1 };

enum class Transport : uint8_t { Wifi = 0, Ble = 1 };

struct RuleInfo {
  const char *id;               // stable snake_case identifier
  Transport transport;
  Strength strength;
  uint8_t upstream_tier;        // flock-you confidence tier (0-4)
  const char *upstream_method;  // flock-you dashboard detection_method key
};

const RuleInfo &rule_info(Rule rule);

// Device types shown to the user, grouping rules by what they indicate.
// Possible types rest on broad identifiers. Append new types before Count;
// families from the parity reference are added with their rules.
enum class DeviceType : uint8_t {
  FlockCamera = 0,       // drive-tested probe fingerprint
  FlockOuiPossible,      // target OUI only
  PenguinBattery,        // Flock Penguin battery pack names
  PenguinBatteryPossible,  // bare serial name or the XUNTONG company ID
  FlockAccessory,        // Flock accessory GATT service
  FlockRaven,            // Raven GATT service range
  NordicDfuPossible,     // generic Nordic DFU name or service
  Count
};

struct DeviceTypeInfo {
  const char *name;  // at most 12 characters, for the OLED; "?" marks possible types
  bool possible;
};

DeviceType device_type(Rule rule);
const DeviceTypeInfo &device_type_info(DeviceType type);

struct Match {
  Rule rule;
  uint8_t mac[6];  // display order, most significant byte first
  char name[33];   // BLE advertised name when present, otherwise empty
};

// ---- Wi-Fi ----------------------------------------------------------------

constexpr size_t kMaxWifiMatchesPerFrame = 3;

// Cheap pre-filter for the capture callback: true when an address field of
// the 802.11 frame carries a target OUI, so the frame is worth copying for
// full matching. `frame` starts at the MAC header.
bool wifi_frame_is_candidate(const uint8_t *frame, size_t len, bool is_mgmt);

// Classifies one captured 802.11 frame, starting at the MAC header and
// possibly including the 4-byte FCS. `is_mgmt` is true when the driver
// delivered it as a management frame. Writes at most `out_cap` matches and
// returns how many were written.
size_t match_wifi_frame(const uint8_t *frame, size_t len, bool is_mgmt, Match *out,
                        size_t out_cap);

// ---- BLE ------------------------------------------------------------------

// Classifies raw advertising data (a sequence of AD structures). `addr` is in
// display order. On a match, fills `out` with the strongest rule that applied:
// any specific rule wins over a possible one, and within a strength the order
// is name, then manufacturer data, then service UUIDs.
bool match_ble_advert(const uint8_t addr[6], const uint8_t *ad, size_t ad_len, Match *out);

// ---- Exposed for tests ----------------------------------------------------

size_t target_oui_count();

// Builds the comma-separated IE fingerprint of a probe-request body (the IEs
// after the MAC header). Returns false when no fingerprint could be built.
bool build_probe_ie_signature(const uint8_t *body, size_t len, char *out, size_t cap);

// True when the body's fingerprint, with or without a trailing FCS, equals
// the drive-tested primary signature.
bool probe_body_has_primary_signature(const uint8_t *body, size_t len);

}  // namespace fy
