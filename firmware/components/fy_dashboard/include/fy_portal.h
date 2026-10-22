// SPDX-License-Identifier: MIT
//
// Captive portal (CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL, the default): the DNS
// answers and HTTP routing that make phones open the status page in their
// "sign in to network" window. Pure C++ with no ESP-IDF dependencies so it can
// be tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::portal {

// Answers given to every name, so cached replies expire soon after leaving.
constexpr uint32_t kDnsTtlSeconds = 10;

// Builds a reply to a DNS `query`: an A question for any name is answered
// with `ipv4` (network byte order); any other type gets an empty answer so
// the client falls back to A. Returns the reply length, or 0 when the packet
// should be ignored (not a single standard query, malformed, or the reply
// does not fit `cap`).
size_t build_dns_reply(const uint8_t *query, size_t len, const uint8_t ipv4[4], uint8_t *out,
                       size_t cap);

enum class Route : uint8_t {
  Page,      // serve the status page
  Redirect,  // redirect to http://<local address>/
};

// Routes an HTTP request in portal mode. Only "/" (with or without a query)
// addressed to the device itself serves the page; everything else, including
// the operating systems' connectivity checks to their own host names, is
// redirected there. `host` is the Host header (may include a port; empty or
// null when absent) and `local_ip` the address the request arrived on.
Route route_request(const char *host, const char *path, const char *local_ip);

}  // namespace fy::portal
