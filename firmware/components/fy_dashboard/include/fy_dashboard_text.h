// SPDX-License-Identifier: MIT
//
// Dashboard text: Wi-Fi credentials, the login and change-password pages,
// and the status as JSON. Pure C++ with no ESP-IDF dependencies so it can be
// tested on a host computer.

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

// ---- Pages ---------------------------------------------------------------------------

enum class LoginNotice : uint8_t {
  None = 0,
  Failed,     // wrong password
  Wait,       // too many attempts; wait `wait_s`
  LoggedOut,
  Expired,    // the session ended (timeout or reboot)
};

// The login form, posting "password" to /login. Returns the length written,
// or 0 (and an empty string) when `cap` is too small. Like every page here:
// no scripts and no external resources.
size_t render_login_page(LoginNotice notice, uint32_t wait_s, char *out, size_t cap);

// The change-password form, posting "new" and "confirm" (and "current"
// unless `first_login`) to /password. `first_login`: the default password is
// still set and must be replaced before anything else. `problem` is shown as
// an error when not empty.
size_t render_password_page(bool first_login, const char *problem, char *out, size_t cap);

// ---- Status --------------------------------------------------------------------------

struct Status {
  const char *board;
  uint32_t uptime_s;
  bool wifi_session;      // this request came over a Wi-Fi session (else USB)
  bool detection_paused;  // a Wi-Fi session is on, which pauses detection
  bool wifi_capture;      // the detector's actual state, whatever the reason
  bool ble_scan;
  uint16_t idle_minutes;  // a Wi-Fi session ends after this long without use

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

  bool storage_available;
  uint8_t storage_percent;
  uint32_t records_used;
  uint32_t records_capacity;
  uint32_t encounter_records;
  uint32_t record_seq;  // changes whenever a record is written
  // The log's history: an erase or a reboot changes (boot, clears), after
  // which a reader's copy is no longer the device's; records before
  // oldest_seq have left the log.
  uint32_t history_boot;
  uint32_t history_clears;
  uint32_t oldest_seq;

  bool passkey_temporary;  // the Wi-Fi passkey is still the generated one

  bool phone_time;  // the clock has been set from a dashboard browser
};

// The status as a JSON object (no location). Returns the length written, or
// 0 when `cap` is too small.
size_t format_status_json(const Status &status, char *out, size_t cap);

}  // namespace fy::dashboard
