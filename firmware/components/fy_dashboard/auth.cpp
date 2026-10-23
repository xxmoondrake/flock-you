// SPDX-License-Identifier: MIT

#include "fy_dashboard_auth.h"

#include <cstring>

namespace fy::dashboard {
namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes one urlencoded value into `out`; false when malformed, when it
// contains a NUL, or when it does not fit.
bool decode_value(const char *p, const char *end, char *out, size_t cap) {
  size_t n = 0;
  while (p < end) {
    int c = static_cast<unsigned char>(*p);
    if (c == '+') {
      c = ' ';
      p++;
    } else if (c == '%') {
      if (end - p < 3) return false;
      const int hi = hex_value(p[1]);
      const int lo = hex_value(p[2]);
      if (hi < 0 || lo < 0) return false;
      c = hi * 16 + lo;
      p += 3;
    } else {
      p++;
    }
    if (c == 0 || n + 1 >= cap) return false;
    out[n++] = static_cast<char>(c);
  }
  out[n] = '\0';
  return true;
}

}  // namespace

PasswordProblem check_new_password(const char *password, const char *confirm,
                                   const char *default_password) {
  if (!password || !*password || !confirm || !*confirm) return PasswordProblem::Missing;
  const size_t len = strlen(password);
  if (len < kMinPasswordLen) return PasswordProblem::TooShort;
  if (len > kMaxPasswordLen) return PasswordProblem::TooLong;
  for (const char *p = password; *p; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c == 0x7f) return PasswordProblem::BadCharacter;
  }
  if (strcmp(password, confirm) != 0) return PasswordProblem::Mismatch;
  if (default_password && strcmp(password, default_password) == 0) {
    return PasswordProblem::SameAsDefault;
  }
  return PasswordProblem::None;
}

const char *describe(PasswordProblem problem) {
  switch (problem) {
    case PasswordProblem::Missing: return "Fill in every field.";
    case PasswordProblem::TooShort: return "Use at least 8 characters.";
    case PasswordProblem::TooLong: return "Use at most 64 characters.";
    case PasswordProblem::BadCharacter: return "Use letters, digits, spaces, and symbols only.";
    case PasswordProblem::Mismatch: return "The two new passwords differ.";
    case PasswordProblem::SameAsDefault: return "Choose a password other than the default.";
    case PasswordProblem::WrongCurrent: return "The current password is wrong.";
    default: return "";
  }
}

bool form_value(const char *body, size_t len, const char *key, char *out, size_t cap) {
  if (!body || !key || !out || cap == 0) return false;
  const size_t key_len = strlen(key);
  const char *end = body + len;
  for (const char *p = body; p < end;) {
    const char *amp = static_cast<const char *>(memchr(p, '&', static_cast<size_t>(end - p)));
    const char *pair_end = amp ? amp : end;
    const char *eq = static_cast<const char *>(memchr(p, '=', static_cast<size_t>(pair_end - p)));
    if (eq && static_cast<size_t>(eq - p) == key_len && memcmp(p, key, key_len) == 0) {
      return decode_value(eq + 1, pair_end, out, cap);
    }
    p = amp ? amp + 1 : end;
  }
  return false;
}

bool constant_time_equal(const void *a, const void *b, size_t len) {
  const uint8_t *x = static_cast<const uint8_t *>(a);
  const uint8_t *y = static_cast<const uint8_t *>(b);
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= static_cast<uint8_t>(x[i] ^ y[i]);
  return diff == 0;
}

// ---- Sessions ----------------------------------------------------------------------

void Sessions::create(RandomByte next, void *ctx, uint32_t now_ms, char *token) {
  Entry *slot = &entries_[0];
  for (Entry &e : entries_) {
    if (!e.used) {
      slot = &e;
      break;
    }
    if (now_ms - e.last_ms > now_ms - slot->last_ms) slot = &e;  // least recently used
  }
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < kTokenLen; i += 2) {
    const uint8_t b = next(ctx);
    slot->token[i] = kHex[b >> 4];
    slot->token[i + 1] = kHex[b & 0x0f];
  }
  slot->token[kTokenLen] = '\0';
  slot->used = true;
  slot->created_ms = slot->last_ms = now_ms;
  memcpy(token, slot->token, kTokenLen + 1);
}

bool Sessions::touch(const char *token, uint32_t now_ms, bool refresh) {
  if (!token || strlen(token) != kTokenLen) return false;
  for (Entry &e : entries_) {
    if (!e.used || !constant_time_equal(e.token, token, kTokenLen)) continue;
    if (now_ms - e.last_ms > kIdleMs || now_ms - e.created_ms > kLifetimeMs) {
      e.used = false;
      return false;
    }
    if (refresh) e.last_ms = now_ms;
    return true;
  }
  return false;
}

void Sessions::end(const char *token) {
  if (!token || strlen(token) != kTokenLen) return;
  for (Entry &e : entries_) {
    if (e.used && constant_time_equal(e.token, token, kTokenLen)) e.used = false;
  }
}

void Sessions::end_all() {
  for (Entry &e : entries_) e.used = false;
}

// ---- Login throttling --------------------------------------------------------------

uint32_t LoginThrottle::wait_ms(uint32_t now_ms) const {
  if (failures_ < kFreeAttempts) return 0;
  uint32_t delay = kBaseDelayMs;
  for (uint32_t i = kFreeAttempts; i < failures_ && delay < kMaxDelayMs; i++) delay *= 2;
  if (delay > kMaxDelayMs) delay = kMaxDelayMs;
  const uint32_t since = now_ms - last_failure_ms_;
  return since >= delay ? 0 : delay - since;
}

void LoginThrottle::failed(uint32_t now_ms) {
  if (failures_ < UINT32_MAX) failures_++;
  last_failure_ms_ = now_ms;
}

}  // namespace fy::dashboard
