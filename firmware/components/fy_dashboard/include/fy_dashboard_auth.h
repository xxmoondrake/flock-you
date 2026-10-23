// SPDX-License-Identifier: MIT
//
// Dashboard login: sessions, login throttling, form parsing, and the password
// rules. Pure C++ with no ESP-IDF dependencies so it can be tested on a host
// computer; the device supplies randomness, time, and password hashing.

#pragma once

#include <cstddef>
#include <cstdint>

#include "fy_dashboard_text.h"

namespace fy::dashboard {

constexpr size_t kMinPasswordLen = 8;
constexpr size_t kMaxPasswordLen = 64;

enum class PasswordProblem : uint8_t {
  None = 0,
  Missing,        // a required field was empty
  TooShort,
  TooLong,
  BadCharacter,   // control characters
  Mismatch,       // new and confirmation differ
  SameAsDefault,  // the build-time default cannot be kept
  WrongCurrent,   // the current password did not verify
};

// Checks a new password and its confirmation against the rules.
PasswordProblem check_new_password(const char *password, const char *confirm,
                                   const char *default_password);

// A sentence for the change-password page; empty for None.
const char *describe(PasswordProblem problem);

// Decodes the value of `key` from an application/x-www-form-urlencoded body
// of `len` bytes ('+' and %XX decoded). Returns false when the key is absent,
// malformed, or its value does not fit `cap` with a terminating NUL.
bool form_value(const char *body, size_t len, const char *key, char *out, size_t cap);

// Compares without an early exit, so timing does not reveal where buffers
// differ.
bool constant_time_equal(const void *a, const void *b, size_t len);

constexpr size_t kTokenLen = 32;  // hex characters: 128 random bits

// Logged-in browsers, identified by a random token in a cookie. Sessions end
// after kIdleMs without a request, kLifetimeMs after login, on logout, or on
// reboot (they are kept in RAM only).
class Sessions {
 public:
  static constexpr size_t kMax = 4;
  static constexpr uint32_t kIdleMs = 30u * 60u * 1000u;
  static constexpr uint32_t kLifetimeMs = 12u * 3600u * 1000u;

  // Starts a session, replacing the least recently used one when all are in
  // use, and writes its token (kTokenLen characters and a NUL) to `token`.
  void create(RandomByte next, void *ctx, uint32_t now_ms, char *token);
  // True when `token` names a live session. Refreshes its idle timer unless
  // `refresh` is false (requests a page makes on its own, such as polling).
  bool touch(const char *token, uint32_t now_ms, bool refresh = true);
  void end(const char *token);
  void end_all();

 private:
  struct Entry {
    bool used = false;
    char token[kTokenLen + 1] = {};
    uint32_t created_ms = 0;
    uint32_t last_ms = 0;
  };
  Entry entries_[kMax];
};

// Slows down password guessing: after kFreeAttempts failures in a row, each
// further attempt waits kBaseDelayMs, doubling up to kMaxDelayMs.
class LoginThrottle {
 public:
  static constexpr uint32_t kFreeAttempts = 5;
  static constexpr uint32_t kBaseDelayMs = 5000;
  static constexpr uint32_t kMaxDelayMs = 300000;

  // Milliseconds until the next attempt is allowed; 0 when allowed now.
  uint32_t wait_ms(uint32_t now_ms) const;
  void failed(uint32_t now_ms);
  void succeeded() { failures_ = 0; }

 private:
  uint32_t failures_ = 0;
  uint32_t last_failure_ms_ = 0;
};

}  // namespace fy::dashboard
