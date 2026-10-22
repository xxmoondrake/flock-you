// SPDX-License-Identifier: MIT

#include "fy_dashboard_text.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fy::dashboard {
namespace {

constexpr unsigned kSymbols = sizeof(kAlphabet) - 1;
// Bytes at or above this would favour the first symbols; they are redrawn.
constexpr unsigned kRejectFrom = 256 - 256 % kSymbols;

// Appends formatted text, tracking overflow instead of truncating silently.
struct Writer {
  char *out;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  void add(const char *fmt, ...) {
    if (overflow) return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(out + len, cap - len, fmt, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= cap - len) {
      overflow = true;
      return;
    }
    len += static_cast<size_t>(n);
  }

  // Appends text with the HTML-special characters escaped.
  void add_escaped(const char *text) {
    for (const char *p = text; *p && !overflow; p++) {
      switch (*p) {
        case '<': add("&lt;"); break;
        case '>': add("&gt;"); break;
        case '&': add("&amp;"); break;
        case '"': add("&quot;"); break;
        default: add("%c", *p); break;
      }
    }
  }
};

}  // namespace

char random_symbol(RandomByte next, void *ctx) {
  for (;;) {
    const unsigned b = next(ctx);
    if (b < kRejectFrom) return kAlphabet[b % kSymbols];
  }
}

bool make_passkey(RandomByte next, void *ctx, char *out, size_t cap) {
  constexpr size_t kLen = 14;  // 12 symbols and two dashes
  if (!out || cap < kLen + 1) return false;
  for (size_t i = 0; i < kLen; i++) out[i] = (i == 4 || i == 9) ? '-' : random_symbol(next, ctx);
  out[kLen] = '\0';
  return true;
}

bool make_ssid(const char *prefix, RandomByte next, void *ctx, char *out, size_t cap) {
  constexpr size_t kSuffix = 4;
  constexpr size_t kMaxSsid = 32;
  const size_t plen = prefix ? strlen(prefix) : 0;
  if (!out || plen + kSuffix > kMaxSsid || plen + kSuffix + 1 > cap) return false;
  if (plen) memcpy(out, prefix, plen);
  for (size_t i = 0; i < kSuffix; i++) {
    char c = random_symbol(next, ctx);
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    out[plen + i] = c;
  }
  out[plen + kSuffix] = '\0';
  return true;
}

size_t render_status_page(const StatusPage &p, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  w.add(
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Detector status</title><style>"
      "body{font-family:sans-serif;margin:16px;max-width:36em}"
      "table{border-collapse:collapse}td,th{padding:2px 10px 2px 0;text-align:left}"
      ".note{color:#555}</style></head><body><h1>Detector status</h1>");
  if (p.detection_paused) {
    w.add(
        "<p><b>Detection is paused</b> while the Wi-Fi dashboard is on. It resumes when the "
        "dashboard is stopped on the device or after %u minutes without use.</p>",
        static_cast<unsigned>(p.idle_minutes));
  } else {
    w.add("<p>Detection is running.</p>");
  }

  w.add("<p>Battery: ");
  if (!p.battery_available) {
    w.add("not measured on this board");
  } else if (!p.battery_valid) {
    w.add("no reading");
  } else {
    w.add("%u%% (%u.%02u V)", static_cast<unsigned>(p.battery_percent), p.battery_mv / 1000u,
          (p.battery_mv % 1000u) / 10u);
  }
  w.add("<br>GPS: ");
  if (!p.gps_available) {
    w.add("none");
  } else if (!p.gps_fix) {
    w.add("no fix");
  } else {
    w.add("%s fix, %u satellites", p.fix_type == 3 ? "3D" : "2D",
          static_cast<unsigned>(p.satellites));
  }
  w.add("<br>Matches reported: %lu (specific %lu)</p>", static_cast<unsigned long>(p.reported),
        static_cast<unsigned long>(p.reported_specific));

  w.add("<h2>Detections</h2>");
  if (p.row_count <= 0) {
    w.add("<p>No detections yet.</p>");
  } else {
    w.add("<table><tr><th>Device type</th><th>This session</th><th>Stored</th></tr>");
    for (int i = 0; i < p.row_count; i++) {
      w.add("<tr><td>");
      w.add_escaped(p.rows[i].name ? p.rows[i].name : "");
      w.add("</td><td>%u</td><td>%u</td></tr>", static_cast<unsigned>(p.rows[i].session),
            static_cast<unsigned>(p.rows[i].stored));
    }
    w.add("</table>");
    if (p.counts_saturated) w.add("<p class=\"note\">Counts are lower bounds.</p>");
    w.add("<p class=\"note\">Types ending in ? rest on broad identifiers.</p>");
  }

  w.add("<h2>Storage</h2><p>");
  if (p.storage_available) {
    w.add("%u%% used (%lu of %lu records)", static_cast<unsigned>(p.storage_percent),
          static_cast<unsigned long>(p.records_used),
          static_cast<unsigned long>(p.records_capacity));
  } else {
    w.add("unavailable");
  }
  const uint32_t hours = p.uptime_s / 3600;
  w.add("<br>Uptime: %luh %02lum</p>", static_cast<unsigned long>(hours),
        static_cast<unsigned long>((p.uptime_s / 60) % 60));

  w.add("<p class=\"note\">Read-only. This page does not refresh by itself; reload it to "
        "update.");
  if (p.passkey_shared) {
    w.add(" The Wi-Fi passkey is the shared one built into this firmware; anyone who knows it "
          "can join while the dashboard is on.");
  } else if (p.passkey_temporary) {
    w.add(" The Wi-Fi passkey is still the generated one shown on the device.");
  }
  w.add("</p></body></html>");

  if (w.overflow) {
    out[0] = '\0';
    return 0;
  }
  return w.len;
}

}  // namespace fy::dashboard
