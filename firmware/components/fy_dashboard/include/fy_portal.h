// SPDX-License-Identifier: MIT
//
// Captive portal (CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL, the default): the DNS
// answers and HTTP routing that make phones open the dashboard in their
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
  Page,      // serve the dashboard
  Redirect,  // redirect to http://<local address>/
};

// True when the Host header `host` names `name` (case-insensitively),
// optionally with a port.
bool host_is(const char *host, const char *name);

// True for the dashboard's own paths ("/", "/login", "/logout", "/password",
// "/app.js", "/app.css", and anything under "/api/" or "/export/"), with or
// without a query.
bool is_dashboard_path(const char *path);

// Routes an HTTP request in portal mode. Only the dashboard's own paths
// addressed to the device itself, by `local_ip` (the address the request
// arrived on) or by the dashboard's host name `name`, are served;
// everything else, including the operating systems' connectivity checks to
// their own host names, is redirected there. `host` is the Host header (may
// include a port; empty or null when absent).
Route route_request(const char *host, const char *path, const char *local_ip, const char *name);

}  // namespace fy::portal
