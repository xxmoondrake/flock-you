// SPDX-License-Identifier: MIT
//
// Wi-Fi and BLE signature matching. Pure C++ with no ESP-IDF dependencies so
// it can be unit-tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy {

// The rule that produced a match. Values are stable identifiers for logs and
// stored records; append new rules before Count, and keep retired rules so
// stored records still decode.
enum class Rule : uint8_t {
  WifiProbeIeSignature = 0,  // Flock radio OUI + wildcard probe + drive-tested IE fingerprint
  WifiWildcardProbe,         // Flock radio OUI + wildcard probe, fingerprint did not match
  WifiOuiTransmitter,        // watchlist OUI in addr2 (transmitter), any frame
  WifiOuiReceiver,           // watchlist OUI in addr1 (receiver), e.g. an AP's probe response
  WifiOuiBssid,              // watchlist OUI in addr3 (BSSID) of a management frame
  BleNamePenguinSerial,      // "Penguin-" followed by exactly 10 digits
  BleNameExtBattery,         // "FS Ext Battery"
  BleNameBareSerial,         // exactly 10 digits; retired 2026-10-22 (matches phones)
  BleNameDfuTarg,            // "DfuTarg", the Nordic legacy DFU bootloader default
  BleMfgXuntong,             // manufacturer data with company ID 0x09C8
  BleServiceFlock,           // Flock accessory 128-bit service UUID
  BleServiceNordicDfu,       // Nordic legacy DFU 128-bit service UUID
  BleServiceRaven,           // Raven 16-bit service UUID 0x3100, 0x3200, ... 0x3500
  WifiSsidFlock,             // SSID "Flock-" followed by 4 or more hex digits
  BleAxonTag,                // "BWCDEVICE" in service UUID or service data bytes
  BleMfgAxon,                // manufacturer data with company ID 0x034D (TASER/Axon)
  BleServiceAxon,            // 16-bit service UUID 0xFE6B, 0xFE6C, or 0xFC81
  BleNameBodyWorn,           // name starting "BodyWorn Remote"
  BleMfgMotorola,            // manufacturer data with company ID 0x04EC
  BleServiceMotorola,        // 16-bit service UUID 0xFD8E or 0xFE04
  BleNameFsHex,              // name "FS-" followed by 4 or more hex digits
  BleNameFlock,              // name starting "Flock"
  BleOui,                    // watchlist OUI as a BLE public device address
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
  Strength strength;            // OUI rules take the matched vendor's strength instead
  uint8_t upstream_tier;        // flock-you confidence tier (0-4)
  const char *upstream_method;  // flock-you dashboard detection_method key
};

const RuleInfo &rule_info(Rule rule);

// What kind of gear a signature indicates.
enum class Category : uint8_t {
  Flock,
  Alpr,
  BodyCamera,
  Radio,
  VehicleNetwork,
  SpeedRadar,
  LightBar,
  Drone,
  GunshotSensor,
};

// Device types shown to the user: a gear category and how strong the
// evidence is. Possible types rest on broad identifiers. Types are derived
// when shown, never stored, so they may be reordered.
enum class DeviceType : uint8_t {
  FlockCamera = 0,         // probe fingerprint, Flock's own OUI, or its SoftAP SSID
  FlockPossible,           // Flock radio-module OUIs, or a "Flock" name
  PenguinBattery,          // Flock Penguin battery pack names
  PenguinBatteryPossible,  // the XUNTONG company ID or an "FS-" name
  FlockAccessory,          // Flock accessory GATT service
  FlockRaven,              // Raven GATT services
  NordicDfuPossible,       // generic Nordic DFU name or service
  Alpr,
  AlprPossible,
  BodyCamera,
  BodyCameraPossible,
  Radio,
  RadioPossible,
  VehicleNetworkPossible,
  SpeedRadar,
  LightBar,
  DronePossible,
  GunshotSensor,
  Count
};

struct DeviceTypeInfo {
  const char *name;   // at most 12 characters, for the OLED; "?" marks possible types
  const char *label;  // full name, for the dashboard and exports
  bool possible;
};

const DeviceTypeInfo &device_type_info(DeviceType type);

// ---- Vendor prefixes --------------------------------------------------------

// Vendors that can be switched off as a group, like the parity reference's
// per-vendor excludes.
enum class VendorGroup : uint8_t { None = 0, Motorola, SierraWireless, Cradlepoint };

// A watchlist entry: an IEEE assignment (MA-L /24, MA-M /28, MA-S or IAB /36)
// registered to a vendor of law-enforcement or surveillance equipment.
struct OuiEntry {
  uint8_t prefix[5];  // the first `bits` bits are significant
  uint8_t bits;       // 24, 28, or 36
  const char *vendor;
  Category category;
  Strength strength;  // specific for vendors that sell essentially only this gear
  VendorGroup group;
};

// The watchlist entry covering `mac`, ignoring the options below (for
// classifying stored records). Null when none does.
const OuiEntry *watchlist_lookup(const uint8_t mac[6]);

// Registrant of a Flock radio-module OUI (Lite-On and others), or null. These
// prefixes are shared with consumer products.
const char *flock_radio_oui_vendor(const uint8_t mac[6]);

struct Options {
  bool motorola = true;         // Motorola Solutions OUIs and BLE IDs
  bool sierra_wireless = true;  // Sierra Wireless OUIs
  bool cradlepoint = false;     // Cradlepoint OUIs; off by default as in the reference
  // Upstream matched the Flock radio-module OUIs on any frame and address
  // field. Off: they are matched only in wildcard probe requests, where the
  // IE fingerprint can tell a camera from a laptop.
  bool radio_ouis_any_frame = false;
};

// Matching options; set once at startup, before matching runs.
void set_options(const Options &options);
const Options &options();

// ---- Matches ------------------------------------------------------------------

struct Match {
  Rule rule;
  Strength strength;  // the rule's, or the matched vendor's for OUI rules
  uint8_t mac[6];     // display order, most significant byte first
  char name[33];      // BLE advertised name or Wi-Fi SSID when present, otherwise empty
};

// How a stored match is presented: its device type, strength, and vendor
// (null when the rule names none).
struct Classification {
  DeviceType type;
  Strength strength;
  const char *vendor;
};

Classification classify(Rule rule, const uint8_t mac[6]);

// ---- Wi-Fi ----------------------------------------------------------------

constexpr size_t kMaxWifiMatchesPerFrame = 4;  // addr2, addr1, addr3, SSID

// Cheap pre-filter for the capture callback: true when an address field of
// the 802.11 frame carries a watched OUI, or a beacon or probe carries a
// "Flock-" SSID, so the frame is worth copying for full matching. `frame`
// starts at the MAC header.
bool wifi_frame_is_candidate(const uint8_t *frame, size_t len, bool is_mgmt);

// Classifies one captured 802.11 frame, starting at the MAC header and
// possibly including the 4-byte FCS. `is_mgmt` is true when the driver
// delivered it as a management frame. Writes at most `out_cap` matches and
// returns how many were written.
size_t match_wifi_frame(const uint8_t *frame, size_t len, bool is_mgmt, Match *out,
                        size_t out_cap);

// ---- BLE ------------------------------------------------------------------

// Classifies raw advertising data (a sequence of AD structures). `addr` is in
// display order; `public_addr` is true for a public device address, the only
// kind whose OUI means anything. On a match, fills `out` with the strongest
// rule that applied: any specific rule wins over a possible one, and within a
// strength the order is name, payload tag, service UUIDs, manufacturer data,
// then the address OUI.
bool match_ble_advert(const uint8_t addr[6], bool public_addr, const uint8_t *ad, size_t ad_len,
                      Match *out);

// ---- Exposed for tests ----------------------------------------------------

size_t flock_radio_oui_count();
size_t watchlist_count();

// Builds the comma-separated IE fingerprint of a probe-request body (the IEs
// after the MAC header). Returns false when no fingerprint could be built.
bool build_probe_ie_signature(const uint8_t *body, size_t len, char *out, size_t cap);

// True when the body's fingerprint, with or without a trailing FCS, equals
// the drive-tested primary signature.
bool probe_body_has_primary_signature(const uint8_t *body, size_t len);

}  // namespace fy
