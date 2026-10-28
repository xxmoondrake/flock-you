// SPDX-License-Identifier: MIT

#include "fy_dashboard_text.h"

#include <cstdlib>
#include <cstring>

#include "fy_dashboard_auth.h"

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
      "a{color:inherit}.foot{margin-top:24px;text-align:center}"
      ".logo{display:block;margin:0 0 12px;border-radius:6px;"
      "image-rendering:pixelated}</style></head><body><main>"
      "<img class=\"logo\" src=\"/logo.png\" alt=\"\" width=\"128\" height=\"64\"><h1>%s</h1>",
      title, title);
}

// Closes a page, after a line naming the firmware when `footer` is given.
void page_end(Writer &w, const char *footer) {
  if (footer && *footer) {
    w.add("<p class=\"note foot\">");
    w.add_html(footer);
    w.add("</p>");
  }
  w.add("</main></body></html>");
}

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

size_t render_login_page(LoginNotice notice, uint32_t wait_s, char *out, size_t cap,
                         const char *footer) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  page_start(w, "Limier");
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
  page_end(w, footer);
  return w.finish();
}

size_t render_password_page(bool first_login, const char *problem, char *out, size_t cap,
                            const char *footer) {
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
  page_end(w, footer);
  return w.finish();
}

// ---- Status --------------------------------------------------------------------------

size_t format_status_json(const Status &s, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  w.add("{\"board\":");
  w.add_json_string(s.board ? s.board : "");
  w.add(",\"model\":");
  w.add_json_string(s.model ? s.model : "");
  w.add(",\"firmware\":");
  w.add_json_string(s.firmware ? s.firmware : "");
  w.add(
      ",\"uptime_s\":%lu,\"dashboard\":\"%s\",\"detection_paused\":%s,\"idle_minutes\":%u,"
      "\"passkey_temporary\":%s",
      static_cast<unsigned long>(s.uptime_s), s.wifi_session ? "wifi" : "usb",
      s.detection_paused ? "true" : "false", static_cast<unsigned>(s.idle_minutes),
      s.passkey_temporary ? "true" : "false");
  w.add(",\"capture\":{\"wifi\":%s,\"ble\":%s}", s.wifi_capture ? "true" : "false",
        s.ble_scan ? "true" : "false");
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
  w.add(",\"record_seq\":%lu,\"history\":{\"boot\":%lu,\"clears\":%lu,\"oldest_seq\":%lu}",
        static_cast<unsigned long>(s.record_seq), static_cast<unsigned long>(s.history_boot),
        static_cast<unsigned long>(s.history_clears), static_cast<unsigned long>(s.oldest_seq));
  w.add(",\"phone\":{\"time\":%s}}", s.phone_time ? "true" : "false");
  return w.finish();
}

size_t export_filename(const char *date, bool json, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  bool dated = date && strlen(date) == 10;
  for (int i = 0; dated && i < 10; i++) {
    dated = (i == 4 || i == 7) ? date[i] == '-' : (date[i] >= '0' && date[i] <= '9');
  }
  Writer w{out, cap};
  w.add("limier-detections%s%s.%s", dated ? "-" : "", dated ? date : "", json ? "json" : "csv");
  return w.finish();
}

size_t format_settings_json(const DeviceSettings &s, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  w.add(
      "{\"alerts\":%s,\"buzzer\":%s,\"flash\":%s,\"tiers\":%u,\"buzzer_fitted\":%s,"
      "\"display\":%s,\"mode\":\"%s\"}",
      s.alerts.enabled ? "true" : "false", s.alerts.buzzer ? "true" : "false",
      s.alerts.flash ? "true" : "false", static_cast<unsigned>(s.alerts.tiers),
      s.buzzer_fitted ? "true" : "false", s.display ? "true" : "false",
      s.standby ? "standby" : "scanning");
  return w.finish();
}

namespace {

// A 0 or 1 field. True when it is absent (keeping *value) or valid.
bool switch_field(const char *body, size_t len, const char *key, bool *value) {
  char v[4];
  if (!form_value(body, len, key, v, sizeof(v))) return true;
  if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) return false;
  *value = v[0] == '1';
  return true;
}

}  // namespace

bool apply_settings_form(const char *body, size_t len, DeviceSettings *settings) {
  DeviceSettings s = *settings;
  if (!switch_field(body, len, "alerts", &s.alerts.enabled) ||
      !switch_field(body, len, "buzzer", &s.alerts.buzzer) ||
      !switch_field(body, len, "flash", &s.alerts.flash)) {
    return false;
  }
  char v[12];
  if (form_value(body, len, "tiers", v, sizeof(v))) {
    char *end = nullptr;
    const unsigned long tiers = strtoul(v, &end, 10);
    if (!v[0] || *end || v[0] == '-' || tiers > fy::alerts::kAllTiers) return false;
    s.alerts.tiers = static_cast<uint8_t>(tiers);
  }
  if (form_value(body, len, "mode", v, sizeof(v))) {
    if (strcmp(v, "scanning") == 0) {
      s.standby = false;
    } else if (strcmp(v, "standby") == 0) {
      s.standby = true;
    } else {
      return false;
    }
  }
  *settings = s;
  return true;
}

}  // namespace fy::dashboard
