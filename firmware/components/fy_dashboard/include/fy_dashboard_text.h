// SPDX-License-Identifier: MIT
//
// Wi-Fi dashboard credentials and the read-only status page. Pure C++ with
// no ESP-IDF dependencies so it can be tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::dashboard {

// Symbols for generated names and passkeys: lowercase letters and digits
// without the easily confused 0, 1, i, l, and o (31 symbols).
constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";

// Source of uniformly random bytes (the hardware RNG on the device).
using RandomByte = uint8_t (*)(void *ctx);

// One symbol drawn uniformly from kAlphabet, rejecting bytes that would bias
// the draw.
char random_symbol(RandomByte next, void *ctx);

// A passkey such as "k7qm-3xtd-ap9w": 12 symbols (about 59 bits) in groups of
// four, easy to read off a small screen and type on a phone. Needs a 15-byte
// buffer; returns false when `cap` is too small.
bool make_passkey(RandomByte next, void *ctx, char *out, size_t cap);

// `prefix` followed by four uppercase symbols, such as "FY-K7QM". Returns
// false when the result would not fit `cap` or a 32-byte SSID.
bool make_ssid(const char *prefix, RandomByte next, void *ctx, char *out, size_t cap);

struct PageRow {
  const char *name;  // device type
  bool possible;
  uint16_t session;  // distinct devices this session
  uint16_t stored;   // distinct devices in stored history
};

struct StatusPage {
  uint32_t uptime_s;
  bool detection_paused;  // a Wi-Fi dashboard session is running
  uint16_t idle_minutes;  // the session ends after this long without use

  bool battery_available;
  bool battery_valid;
  uint16_t battery_mv;
  uint8_t battery_percent;

  bool gps_available;
  bool gps_fix;
  uint8_t fix_type;  // 2 = 2D, 3 = 3D
  uint8_t satellites;

  uint32_t reported;
  uint32_t reported_specific;
  const PageRow *rows;
  int row_count;
  bool counts_saturated;

  bool storage_available;
  uint8_t storage_percent;
  uint32_t records_used;
  uint32_t records_capacity;

  bool passkey_temporary;  // still the generated or built-in passkey
  bool passkey_shared;     // the build-time passkey shared by devices without a display
};

// Renders the status page as a complete HTML document: no scripts, no
// external resources, no automatic refresh, and no location. Returns the
// length written, or 0 (and an empty string) when `cap` is too small.
size_t render_status_page(const StatusPage &page, char *out, size_t cap);

}  // namespace fy::dashboard
