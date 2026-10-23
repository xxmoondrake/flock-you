// SPDX-License-Identifier: MIT

#include "fy_dashboard_text.h"

#include <cstring>

#include "writer.h"

namespace fy::dashboard {
namespace {

constexpr unsigned kSymbols = sizeof(kAlphabet) - 1;
// Bytes at or above this would favour the first symbols; they are redrawn.
constexpr unsigned kRejectFrom = 256 - 256 % kSymbols;

void page_start(Writer &w, const char *title) {
  w.add(
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>%s</title><style>"
      ":root{color-scheme:light dark;--fg:#1d2329;--bg:#f6f7f8;--card:#fff;--line:#d5d9dd;"
      "--accent:#c62828;--muted:#5d6670}"
      "@media(prefers-color-scheme:dark){:root{--fg:#e6e8ea;--bg:#15181b;--card:#1f2327;"
      "--line:#363c42;--accent:#ef5350;--muted:#9aa3ab}}"
      "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,sans-serif}"
      "main{max-width:24rem;margin:10vh auto 0;padding:0 16px}"
      "form{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:16px}"
      "label{display:block;margin:12px 0 4px;font-weight:600}label:first-child{margin-top:0}"
      "input{box-sizing:border-box;width:100%%;padding:10px;font:inherit;border:1px solid "
      "var(--line);border-radius:6px;background:var(--bg);color:var(--fg)}"
      "button{margin-top:16px;width:100%%;padding:10px;font:inherit;font-weight:600;border:0;"
      "border-radius:6px;background:var(--accent);color:#fff}"
      ".err{color:var(--accent);font-weight:600}.note{color:var(--muted);font-size:14px}"
      "a{color:inherit}</style></head><body><main><h1>%s</h1>",
      title, title);
}

void page_end(Writer &w) { w.add("</main></body></html>"); }

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

// ---- Pages ---------------------------------------------------------------------------

size_t render_login_page(LoginNotice notice, uint32_t wait_s, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  page_start(w, "Detector");
  switch (notice) {
    case LoginNotice::Failed: w.add("<p class=\"err\">Wrong password.</p>"); break;
    case LoginNotice::Wait:
      w.add("<p class=\"err\">Too many wrong passwords. Try again in %lu s.</p>",
            static_cast<unsigned long>(wait_s));
      break;
    case LoginNotice::LoggedOut: w.add("<p>Logged out.</p>"); break;
    case LoginNotice::Expired: w.add("<p>Your session ended. Log in again.</p>"); break;
    default: break;
  }
  w.add(
      "<form method=\"post\" action=\"/login\"><label for=\"pw\">Password</label>"
      "<input id=\"pw\" name=\"password\" type=\"password\" autocomplete=\"current-password\" "
      "required autofocus><button type=\"submit\">Log in</button></form>"
      "<p class=\"note\">The same password opens the USB and Wi-Fi dashboards.</p>");
  page_end(w);
  return w.finish();
}

size_t render_password_page(bool first_login, const char *problem, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  page_start(w, first_login ? "Set a new password" : "Change password");
  if (first_login) {
    w.add(
        "<p>This detector still has the default password. Choose a new one to continue; it "
        "replaces the default for both the USB and Wi-Fi dashboards.</p>");
  }
  if (problem && *problem) {
    w.add("<p class=\"err\">");
    w.add_html(problem);
    w.add("</p>");
  }
  w.add("<form method=\"post\" action=\"/password\">");
  if (!first_login) {
    w.add(
        "<label for=\"cur\">Current password</label><input id=\"cur\" name=\"current\" "
        "type=\"password\" autocomplete=\"current-password\" required>");
  }
  w.add(
      "<label for=\"new\">New password</label><input id=\"new\" name=\"new\" type=\"password\" "
      "autocomplete=\"new-password\" minlength=\"8\" maxlength=\"64\" required%s>"
      "<label for=\"conf\">Confirm new password</label><input id=\"conf\" name=\"confirm\" "
      "type=\"password\" autocomplete=\"new-password\" minlength=\"8\" maxlength=\"64\" required>"
      "<button type=\"submit\">Save password</button></form>"
      "<p class=\"note\">8 to 64 characters.</p>",
      first_login ? " autofocus" : "");
  if (!first_login) w.add("<p><a href=\"/\">Back to the dashboard</a></p>");
  page_end(w);
  return w.finish();
}

// ---- Status --------------------------------------------------------------------------

size_t format_status_json(const Status &s, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  w.add("{\"board\":");
  w.add_json_string(s.board ? s.board : "");
  w.add(
      ",\"uptime_s\":%lu,\"dashboard\":\"%s\",\"detection_paused\":%s,\"idle_minutes\":%u,"
      "\"passkey_temporary\":%s",
      static_cast<unsigned long>(s.uptime_s), s.wifi_session ? "wifi" : "usb",
      s.detection_paused ? "true" : "false", static_cast<unsigned>(s.idle_minutes),
      s.passkey_temporary ? "true" : "false");
  w.add(",\"battery\":");
  if (s.battery_available && s.battery_valid) {
    w.add("{\"mv\":%u,\"percent\":%u}", static_cast<unsigned>(s.battery_mv),
          static_cast<unsigned>(s.battery_percent));
  } else {
    w.add("null");
  }
  w.add(",\"gps\":");
  if (!s.gps_available) {
    w.add("null");
  } else {
    w.add("{\"fix\":%s,\"fix_type\":%u,\"satellites\":%u}", s.gps_fix ? "true" : "false",
          static_cast<unsigned>(s.fix_type), static_cast<unsigned>(s.satellites));
  }
  w.add(",\"matches\":{\"reported\":%lu,\"specific\":%lu}",
        static_cast<unsigned long>(s.reported), static_cast<unsigned long>(s.reported_specific));
  w.add(",\"storage\":");
  if (!s.storage_available) {
    w.add("null");
  } else {
    w.add("{\"percent\":%u,\"used\":%lu,\"capacity\":%lu,\"encounter_records\":%lu}",
          static_cast<unsigned>(s.storage_percent), static_cast<unsigned long>(s.records_used),
          static_cast<unsigned long>(s.records_capacity),
          static_cast<unsigned long>(s.encounter_records));
  }
  w.add(",\"record_seq\":%lu,\"phone\":{\"time\":%s}}",
        static_cast<unsigned long>(s.record_seq), s.phone_time ? "true" : "false");
  return w.finish();
}

}  // namespace fy::dashboard
