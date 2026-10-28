// SPDX-License-Identifier: MIT
//
// Host unit tests for the captive-portal test option: pio test -e native

#include <unity.h>

#include <cstring>
#include <vector>

#include "fy_portal.h"

using namespace fy::portal;

void setUp() {}
void tearDown() {}

namespace {

const uint8_t kIp[4] = {192, 168, 7, 1};

// A standard query for `name` with the given type, recursion desired, and an
// optional EDNS OPT record in the additional section.
std::vector<uint8_t> query(const char *name, uint16_t type, bool edns = false) {
  std::vector<uint8_t> q = {0xab, 0xcd, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
                            0x00, 0x00, 0x00, static_cast<uint8_t>(edns ? 1 : 0)};
  const char *p = name;
  while (*p) {
    const char *dot = strchr(p, '.');
    const size_t n = dot ? static_cast<size_t>(dot - p) : strlen(p);
    q.push_back(static_cast<uint8_t>(n));
    q.insert(q.end(), p, p + n);
    p += n;
    if (*p == '.') p++;
  }
  q.push_back(0);
  q.push_back(static_cast<uint8_t>(type >> 8));
  q.push_back(static_cast<uint8_t>(type));
  q.push_back(0);
  q.push_back(1);  // class IN
  if (edns) {
    const uint8_t opt[] = {0x00, 0x00, 0x29, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    q.insert(q.end(), opt, opt + sizeof(opt));
  }
  return q;
}

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

}  // namespace

void test_a_query_is_answered_with_device_address() {
  const auto q = query("connectivitycheck.gstatic.com", 1);
  uint8_t r[512];
  const size_t n = build_dns_reply(q.data(), q.size(), kIp, r, sizeof(r));
  TEST_ASSERT_EQUAL_UINT(q.size() + 16, n);
  TEST_ASSERT_EQUAL_HEX8(0xab, r[0]);  // same ID
  TEST_ASSERT_EQUAL_HEX8(0xcd, r[1]);
  TEST_ASSERT_EQUAL_HEX8(0x85, r[2]);  // response, authoritative, recursion desired
  TEST_ASSERT_EQUAL_HEX8(0x80, r[3]);  // no error
  TEST_ASSERT_EQUAL_UINT16(1, be16(r + 4));
  TEST_ASSERT_EQUAL_UINT16(1, be16(r + 6));
  TEST_ASSERT_EQUAL_MEMORY(q.data() + 12, r + 12, q.size() - 12);  // question echoed
  const uint8_t *a = r + q.size();
  TEST_ASSERT_EQUAL_HEX8(0xc0, a[0]);
  TEST_ASSERT_EQUAL_HEX8(0x0c, a[1]);
  TEST_ASSERT_EQUAL_UINT16(1, be16(a + 2));   // A
  TEST_ASSERT_EQUAL_UINT16(1, be16(a + 4));   // IN
  TEST_ASSERT_EQUAL_UINT16(0, be16(a + 6));   // TTL high
  TEST_ASSERT_EQUAL_UINT16(kDnsTtlSeconds, be16(a + 8));
  TEST_ASSERT_EQUAL_UINT16(4, be16(a + 10));
  TEST_ASSERT_EQUAL_MEMORY(kIp, a + 12, 4);
}

void test_other_types_get_an_empty_answer() {
  const auto q = query("captive.apple.com", 28);  // AAAA
  uint8_t r[512];
  const size_t n = build_dns_reply(q.data(), q.size(), kIp, r, sizeof(r));
  TEST_ASSERT_EQUAL_UINT(q.size(), n);
  TEST_ASSERT_EQUAL_HEX8(0x80, r[3]);  // no error, no data
  TEST_ASSERT_EQUAL_UINT16(0, be16(r + 6));
}

void test_edns_record_is_dropped() {
  const auto q = query("www.msftconnecttest.com", 1, true);
  uint8_t r[512];
  const size_t n = build_dns_reply(q.data(), q.size(), kIp, r, sizeof(r));
  TEST_ASSERT_EQUAL_UINT(q.size() - 11 + 16, n);
  TEST_ASSERT_EQUAL_UINT16(0, be16(r + 10));  // no additional records
}

void test_bad_packets_are_ignored() {
  uint8_t r[512];
  auto q = query("example.com", 1);
  // Too short, a response, a non-standard opcode, two questions.
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(q.data(), 11, kIp, r, sizeof(r)));
  auto response = q;
  response[2] |= 0x80;
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(response.data(), response.size(), kIp, r, sizeof(r)));
  auto opcode = q;
  opcode[2] |= 0x10;
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(opcode.data(), opcode.size(), kIp, r, sizeof(r)));
  auto two = q;
  two[5] = 2;
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(two.data(), two.size(), kIp, r, sizeof(r)));
  // A compression pointer in the question, a truncated name, a missing type.
  auto pointer = q;
  pointer[12] = 0xc0;
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(pointer.data(), pointer.size(), kIp, r, sizeof(r)));
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(q.data(), 16, kIp, r, sizeof(r)));
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(q.data(), q.size() - 2, kIp, r, sizeof(r)));
  // A reply that would not fit.
  TEST_ASSERT_EQUAL_UINT(0, build_dns_reply(q.data(), q.size(), kIp, r, q.size()));
}

void test_routing() {
  const char *ip = "192.168.7.1";
  const char *kName = "dash.fy";
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1:80", "/?r=1", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("", "/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request(nullptr, "/", ip, kName));
  // Connectivity checks and other names or paths go to the page.
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("connectivitycheck.gstatic.com", "/generate_204", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("captive.apple.com", "/hotspot-detect.html", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("www.msftconnecttest.com", "/connecttest.txt", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("captive.apple.com", "/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("192.168.7.10", "/", ip, kName));  // prefix only
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("192.168.7.1", "/generate_204", ip, kName));
  // The dashboard's own paths are served; look-alikes are not.
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/login", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/app.js?v=2", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/logo.png", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/favicon.ico", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/manifest.webmanifest", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/icon-192.png", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("captive.apple.com", "/favicon.ico", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/api/detections", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("192.168.7.1", "/export/detections.csv", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("192.168.7.1", "/api/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("192.168.7.1", "/login.php", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("captive.apple.com", "/login", ip, kName));
  // The dashboard's host name counts as the device, in any case and with a port.
  TEST_ASSERT_EQUAL(Route::Page, route_request("dash.fy", "/login", ip, kName));
  TEST_ASSERT_EQUAL(Route::Page, route_request("DASH.fy:443", "/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("dash.fyi", "/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("x.dash.fy", "/", ip, kName));
  TEST_ASSERT_EQUAL(Route::Redirect, route_request("dash.fy", "/generate_204", ip, kName));
}

void test_host_is() {
  TEST_ASSERT_TRUE(host_is("dash.fy", "dash.fy"));
  TEST_ASSERT_TRUE(host_is("Dash.FY:8443", "dash.fy"));
  TEST_ASSERT_FALSE(host_is("dash.f", "dash.fy"));
  TEST_ASSERT_FALSE(host_is("dash.fy.", "dash.fy"));
  TEST_ASSERT_FALSE(host_is("", "dash.fy"));
  TEST_ASSERT_FALSE(host_is("dash.fy", ""));
  TEST_ASSERT_FALSE(host_is(nullptr, "dash.fy"));
  TEST_ASSERT_FALSE(host_is("dash.fy", nullptr));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_a_query_is_answered_with_device_address);
  RUN_TEST(test_other_types_get_an_empty_answer);
  RUN_TEST(test_edns_record_is_dropped);
  RUN_TEST(test_bad_packets_are_ignored);
  RUN_TEST(test_routing);
  RUN_TEST(test_host_is);
  return UNITY_END();
}
