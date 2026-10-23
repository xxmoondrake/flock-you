// SPDX-License-Identifier: MIT

#include "fy_portal.h"

#include <cstring>

namespace fy::portal {
namespace {

constexpr size_t kHeaderLen = 12;
constexpr size_t kMaxNameLen = 255;
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kClassIn = 1;
constexpr size_t kAnswerLen = 16;  // name pointer, type, class, TTL, length, address

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

}  // namespace

size_t build_dns_reply(const uint8_t *q, size_t len, const uint8_t ipv4[4], uint8_t *out,
                       size_t cap) {
  if (!q || !ipv4 || !out || len < kHeaderLen) return 0;
  const bool is_response = q[2] & 0x80;
  const uint8_t opcode = (q[2] >> 3) & 0x0f;
  if (is_response || opcode != 0 || be16(q + 4) != 1) return 0;

  // The question's name: plain labels only (queries do not compress).
  size_t i = kHeaderLen;
  size_t name_len = 0;
  for (;;) {
    if (i >= len) return 0;
    const uint8_t label = q[i];
    if (label == 0) {
      i++;
      break;
    }
    if (label & 0xc0) return 0;
    name_len += label + 1u;
    if (name_len > kMaxNameLen || i + 1 + label > len) return 0;
    i += 1 + label;
  }
  if (i + 4 > len) return 0;
  const uint16_t qtype = be16(q + i);
  const uint16_t qclass = be16(q + i + 2);
  const size_t question_end = i + 4;
  const bool answer = qtype == kTypeA && qclass == kClassIn;
  const size_t reply_len = question_end + (answer ? kAnswerLen : 0);
  if (reply_len > cap) return 0;

  // Header and question copied; additional records (EDNS) are dropped.
  memcpy(out, q, question_end);
  out[2] = static_cast<uint8_t>(0x80 | (q[2] & 0x78) | 0x04 | (q[2] & 0x01));  // QR, AA, RD
  out[3] = 0x80;                                                             // RA, no error
  put16(out + 4, 1);
  put16(out + 6, answer ? 1 : 0);
  put16(out + 8, 0);
  put16(out + 10, 0);
  if (answer) {
    uint8_t *a = out + question_end;
    a[0] = 0xc0;  // pointer to the question's name at offset 12
    a[1] = 0x0c;
    put16(a + 2, kTypeA);
    put16(a + 4, kClassIn);
    put16(a + 6, static_cast<uint16_t>(kDnsTtlSeconds >> 16));
    put16(a + 8, static_cast<uint16_t>(kDnsTtlSeconds));
    put16(a + 10, 4);
    memcpy(a + 12, ipv4, 4);
  }
  return reply_len;
}

bool host_is(const char *host, const char *name) {
  if (!host || !name || !*name) return false;
  size_t i = 0;
  for (; name[i]; i++) {
    const char a = host[i] >= 'A' && host[i] <= 'Z' ? static_cast<char>(host[i] + 32) : host[i];
    const char b = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] + 32) : name[i];
    if (a != b) return false;
  }
  return host[i] == '\0' || host[i] == ':';
}

Route route_request(const char *host, const char *path, const char *local_ip, const char *name) {
  if (host && *host && !host_is(host, local_ip) && !host_is(host, name)) return Route::Redirect;
  return is_dashboard_path(path) ? Route::Page : Route::Redirect;
}

bool is_dashboard_path(const char *path) {
  if (!path || path[0] != '/') return false;
  const size_t len = strcspn(path, "?");
  static const char *const kExact[] = {"/", "/login", "/logout", "/password", "/app.js", "/app.css"};
  for (const char *p : kExact) {
    if (strlen(p) == len && strncmp(path, p, len) == 0) return true;
  }
  static const char *const kPrefixes[] = {"/api/", "/export/"};
  for (const char *p : kPrefixes) {
    const size_t n = strlen(p);
    if (len > n && strncmp(path, p, n) == 0) return true;
  }
  return false;
}

}  // namespace fy::portal
