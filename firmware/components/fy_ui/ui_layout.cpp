// SPDX-License-Identifier: MIT
//
// Screens (6x8 character cells, 21 columns):
//
// 0 Home
//   row 0  battery (percent and voltage) or the title, and UTC time or
//          uptime; an inverted banner replaces it after a specific match
//   row 1  Wi-Fi and BLE capture state; inverted in standby or while the
//          Wi-Fi dashboard runs, since neither detects
//   row 2  GPS state
//   row 3  current coordinates while the fix is current; otherwise receiver
//          diagnostics (satellites in view, with signal, antenna state)
//   ----   separator
//   row 4  match counts, or an inverted storage warning when nearly full
//   row 5  last match: strength, age, RSSI
//   row 6  last match rule (truncated to the screen width)
//
// 1 Detections: distinct devices per device type, this session and stored.
// 2 Data management: storage use and history; hold for the data menu.
// 3 Device management: mode and dashboards; hold for the device menu. During
//   a Wi-Fi session it shows the SSID, the passkey (only while it is still
//   the generated one), and the page address; during a USB session, the
//   page address.
//
// Menus, the erase confirmation, erase progress, and the power-off prompt
// replace the screen while active. Transient notices use the bottom row.

#include "fy_ui_layout.h"

#include "splash_frame.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace fy::ui {
namespace {

using display::Canvas;
using display::kCharWidth;
using display::kColumns;
using display::kWidth;

constexpr int kRowY[] = {0, 9, 18, 27, 38, 47, 56};
constexpr int kSeparatorY = 36;
// Rows below an inverted title bar on list screens.
constexpr int kListY[] = {11, 20, 29, 38, 47, 56};
constexpr int kListRows = 6;

void text_at(Canvas &c, int y, const char *text, bool inverted = false) {
  char buf[kColumns + 1];
  snprintf(buf, sizeof(buf), "%s", text);  // truncate to the screen width
  if (inverted) {
    c.fill_rect(0, y - 1, kWidth, 9, true);
    c.draw_text(1, y, buf, false);
  } else {
    c.draw_text(0, y, buf);
  }
}

// Centres `text` on the row at `y`, `scale` times the font size; text wider
// than the screen starts at its left edge and is clipped.
void centred(Canvas &c, int y, const char *text, int scale = 1) {
  const int width = static_cast<int>(strlen(text)) * kCharWidth * scale - scale;
  c.draw_text(width < kWidth ? (kWidth - width) / 2 : 0, y, text, true, scale);
}

void line(Canvas &c, int row, const char *text, bool inverted = false) {
  text_at(c, kRowY[row], text, inverted);
}

void right_aligned(Canvas &c, int y, const char *text, bool inverted = false) {
  const int x = kWidth - static_cast<int>(strlen(text)) * kCharWidth;
  c.draw_text(x < 0 ? 0 : x, y, text, !inverted);
}

void title(Canvas &c, const char *left, const char *right = nullptr) {
  c.fill_rect(0, 0, kWidth, 9, true);
  c.draw_text(1, 1, left, false);
  if (right) right_aligned(c, 1, right, true);
}

void gps_line(const UiModel &m, char *out, int cap) {
  if (!m.gps_available) {
    snprintf(out, cap, "GPS none");
  } else if (!m.gps_receiving) {
    snprintf(out, cap, "GPS no data");
  } else if (m.gps_position_current) {
    const char *fix = m.fix_type == 3 ? "3D" : m.fix_type == 2 ? "2D" : "fix";
    if (m.hdop_valid) {
      snprintf(out, cap, "GPS %s sats %u h%.1f", fix, m.satellites, m.hdop);
    } else {
      snprintf(out, cap, "GPS %s sats %u", fix, m.satellites);
    }
  } else if (m.gps_position_ever) {
    char age[8];
    format_age(m.gps_position_age_ms, age, sizeof(age));
    snprintf(out, cap, "GPS stale %s", age);
  } else {
    snprintf(out, cap, "GPS searching");
  }
}

// "view 12 sig 5 ant OK": satellites in view, how many have signal, and the
// receiver's antenna report.
void gps_diagnostics_line(const UiModel &m, char *out, int cap) {
  static const char *const kAntenna[] = {"?", "OK", "OPEN", "SHORT"};
  const char *ant = m.antenna < 4 ? kAntenna[m.antenna] : "?";
  snprintf(out, cap, "view %u sig %u ant %s", m.sats_in_view, m.sats_with_signal, ant);
}

void format_date(uint32_t utc, char *out, int cap) {
  const time_t t = static_cast<time_t>(utc);
  struct tm tm_utc;
#ifdef _WIN32
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif
  snprintf(out, cap, "%04d-%02d-%02d", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
           tm_utc.tm_mday);
}

void render_menu(Canvas &c, const UiModel &m, const UiState &s) {
  c.clear();
  size_t count = 0;
  const char *const *items = menu_items(s.overlay, &count);
  int y = 13;
  int step = 10;
  if (count > 4) {  // five items fit only closer together
    y = 12;
    step = 9;
  }
  if (s.overlay == Overlay::EraseConfirm) {
    title(c, "ERASE ALL RECORDS?");
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu records", static_cast<unsigned long>(m.records_used));
    text_at(c, 11, buf);
    y = 31;
  } else if (s.overlay == Overlay::DeviceMenu) {
    title(c, "DEVICE MENU");
  } else if (s.overlay == Overlay::DashboardMenu) {
    title(c, "START DASHBOARD");
  } else if (s.overlay == Overlay::AlertsMenu) {
    title(c, "ALERTS");
  } else {
    title(c, "DATA MENU");
  }
  for (size_t i = 0; i < count; i++, y += step) {
    const char *label = items[i];
    char buf[kColumns + 1];
    if (s.overlay == Overlay::AlertsMenu && i < 3) {
      // Each setting with its value; the buzzer needs a configured pin.
      const bool on = i == 0 ? m.alerts_enabled : i == 1 ? m.alert_buzzer : m.alert_flash;
      snprintf(buf, sizeof(buf), "%s: %s%s", items[i], on ? "On" : "Off",
               i == 1 && !m.buzzer_fitted ? " (none)" : "");
      label = buf;
    }
    if (s.overlay == Overlay::DeviceMenu) {
      // Mark the selected mode; name the dashboard item by what it will do.
      if (i == 0 && !m.standby) label = "Scanning - selected";
      if (i == 1 && m.standby) label = "Standby - selected";
      if (i == 2) label = m.dashboard_active ? "Stop dashboard" : "Start dashboard";
    }
    text_at(c, y, label, i == s.item);
  }
  text_at(c, 56, "press:next hold:ok");
}

void render_erasing(Canvas &c, const UiModel &m) {
  c.clear();
  title(c, "ERASING RECORDS");
  c.fill_rect(4, 24, 120, 1, true);
  c.fill_rect(4, 35, 120, 1, true);
  c.fill_rect(4, 24, 1, 12, true);
  c.fill_rect(123, 24, 1, 12, true);
  c.fill_rect(6, 26, 116 * m.erase_percent / 100, 8, true);
  char buf[16];
  snprintf(buf, sizeof(buf), "%u%%", m.erase_percent);
  text_at(c, 40, buf);
  text_at(c, 56, "Do not power off");
}

}  // namespace

void render_low_battery(Canvas &c, uint16_t mv, bool starting) {
  c.clear();
  text_at(c, 12, "     LOW BATTERY", true);
  char buf[32];
  snprintf(buf, sizeof(buf), "Battery %u.%02uV", mv / 1000, (mv % 1000) / 10);
  text_at(c, 28, buf);
  text_at(c, 40, starting ? "Too low to start" : "Saving, powering off");
  text_at(c, 52, "Charge to restart");
}

void render_shutdown(Canvas &c) {
  c.load_frame(kSplashFrame);
  c.fill_rect(0, 55, kWidth, 9, false);  // a clear row for the notice
  centred(c, 56, "Saving, powering off");
}

void render_about(Canvas &c, const char *model, const char *version) {
  c.clear();
  centred(c, 6, "Limier", 3);
  char buf[48];
  snprintf(buf, sizeof(buf), "HW %s", model);
  centred(c, 36, buf);
  // A version too long to share the row with its label is shown alone.
  snprintf(buf, sizeof(buf), "FW %s", version);
  centred(c, 48, strlen(buf) <= static_cast<size_t>(kColumns) ? buf : version);
}

namespace {

void render_power_off(Canvas &c) {
  c.clear();
  text_at(c, 20, " RELEASE TO POWER OFF", true);
  text_at(c, 40, "Press PRG to wake");
}

}  // namespace

void format_age(uint32_t ms, char *out, int cap) {
  const uint32_t s = ms / 1000;
  if (s < 60) {
    snprintf(out, cap, "%lus", static_cast<unsigned long>(s));
  } else if (s < 3600) {
    snprintf(out, cap, "%lum", static_cast<unsigned long>(s / 60));
  } else if (s < 86400) {
    snprintf(out, cap, "%luh", static_cast<unsigned long>(s / 3600));
  } else {
    snprintf(out, cap, "%lud", static_cast<unsigned long>(s / 86400));
  }
}

void render_home(Canvas &c, const UiModel &m) {
  c.clear();
  char buf[64];

  // Row 0: a specific match takes over the title row for a few seconds.
  if (m.alert_active) {
    char age[8];
    format_age(m.alert_age_ms, age, sizeof(age));
    snprintf(buf, sizeof(buf), "SPECIFIC MATCH %s", age);
    line(c, 0, buf, true);
  } else {
    if (m.battery_valid) {
      // "87% 4.05V": the estimate first, the measurement it rests on second.
      snprintf(buf, sizeof(buf), "%u%% %u.%02uV", m.battery_percent, m.battery_mv / 1000,
               (m.battery_mv % 1000) / 10);
      line(c, 0, buf);
    } else if (m.battery_available) {
      line(c, 0, "BAT --");
    } else {
      line(c, 0, "FLOCK-YOU");
    }
    if (m.gps_time_valid) {
      // Advance the last receiver time by its age so the clock keeps ticking
      // between sentences.
      uint32_t sod = m.utc_hour * 3600u + m.utc_minute * 60u + m.utc_second +
                     m.gps_time_age_ms / 1000;
      sod %= 86400;
      snprintf(buf, sizeof(buf), "%02lu:%02lu:%02luZ", static_cast<unsigned long>(sod / 3600),
               static_cast<unsigned long>((sod / 60) % 60), static_cast<unsigned long>(sod % 60));
    } else {
      char age[8];
      format_age(m.now_ms, age, sizeof(age));
      snprintf(buf, sizeof(buf), "up %s", age);
    }
    right_aligned(c, kRowY[0], buf);
  }

  // Row 1: capture state. Standby and the dashboard are shown inverted so the
  // screen never implies detection that is not running.
  if (m.dashboard_active && !m.dashboard_usb) {
    line(c, 1, "PAUSED FOR DASHBOARD", true);
  } else if (m.standby) {
    line(c, 1, "STANDBY: NOT SCANNING", true);
  } else {
    const char *ble = m.ble_scan ? "scan" : "off";
    if (m.wifi_capture) {
      snprintf(buf, sizeof(buf), "WiFi ch%-2u  BLE %s", m.channel, ble);
    } else {
      snprintf(buf, sizeof(buf), "WiFi PAUSED BLE %s", ble);
    }
    line(c, 1, buf);
  }

  // Rows 2-3: GPS. Coordinates only while the fix is current; otherwise the
  // receiver diagnostics explain why there is no fix.
  gps_line(m, buf, sizeof(buf));
  line(c, 2, buf);
  if (m.gps_position_current) {
    snprintf(buf, sizeof(buf), "%.5f %.5f", m.latitude_deg, m.longitude_deg);
    line(c, 3, buf);
  } else if (m.gps_available && m.gps_receiving) {
    gps_diagnostics_line(m, buf, sizeof(buf));
    line(c, 3, buf);
  }

  c.fill_rect(0, kSeparatorY, kWidth, 1, true);

  // Rows 4-6: matches. A nearly full store takes over row 4 until offloaded.
  if (m.storage_warning) {
    snprintf(buf, sizeof(buf), "STORAGE %u%% OFFLOAD", m.storage_percent);
    line(c, 4, buf, true);
  } else {
    snprintf(buf, sizeof(buf), "Matches %lu spec %lu", static_cast<unsigned long>(m.reported),
             static_cast<unsigned long>(m.reported_specific));
    line(c, 4, buf);
  }
  if (m.last_valid) {
    char age[8];
    format_age(m.now_ms - m.last_at_ms, age, sizeof(age));
    snprintf(buf, sizeof(buf), "%s %s %ddBm", m.last_specific ? "SPECIFIC" : "possible", age,
             m.last_rssi);
    line(c, 5, buf);
    line(c, 6, m.last_rule ? m.last_rule : "");
  } else {
    line(c, 5, "No matches yet");
  }
}

void render_detections(Canvas &c, const UiModel &m) {
  c.clear();
  // Column headings sit in the title bar: name (12) | session (3) | stored (4).
  // + lower bounds, ~ stored counts being recounted.
  char heading[24];
  snprintf(heading, sizeof(heading), "DETECTIONS%c%c ses  all", m.counts_saturated ? '+' : ' ',
           m.stored_pending ? '~' : ' ');
  title(c, heading);
  if (m.detection_rows == 0) {
    text_at(c, kListY[1], "No detections yet");
    return;
  }
  char buf[32];
  const int shown = m.detection_rows > kListRows ? kListRows - 1 : m.detection_rows;
  for (int i = 0; i < shown; i++) {
    const DetectionRow &r = m.detections[i];
    snprintf(buf, sizeof(buf), "%-12.12s %3u %4u", r.name, r.session, r.stored);
    text_at(c, kListY[i], buf);
  }
  if (shown < m.detection_rows) {
    snprintf(buf, sizeof(buf), "+%d more types", m.detection_rows - shown);
    text_at(c, kListY[kListRows - 1], buf);
  }
}

void render_data(Canvas &c, const UiModel &m) {
  c.clear();
  title(c, "DATA MANAGEMENT");
  if (!m.storage_available) {
    text_at(c, kListY[1], "Storage unavailable");
    return;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "Records %lu/%lu", static_cast<unsigned long>(m.records_used),
           static_cast<unsigned long>(m.records_capacity));
  text_at(c, kListY[0], buf);
  snprintf(buf, sizeof(buf), "Used %u%%", m.storage_percent);
  text_at(c, kListY[1], buf, m.storage_warning);
  snprintf(buf, sizeof(buf), "Encounter recs %lu", static_cast<unsigned long>(m.encounter_records));
  text_at(c, kListY[2], buf);
  if (m.oldest_utc) {
    char date[16];
    format_date(m.oldest_utc, date, sizeof(date));
    snprintf(buf, sizeof(buf), "Oldest %s", date);
  } else {
    snprintf(buf, sizeof(buf), "Oldest: no UTC yet");
  }
  text_at(c, kListY[3], buf);
  text_at(c, kListY[5], "Hold: data menu");
}

void render_device(Canvas &c, const UiModel &m) {
  c.clear();
  title(c, "DEVICE MANAGEMENT");
  char buf[96];
  const char *mode = m.standby ? "Standby" : "Scanning";
  // A Wi-Fi session pauses detection; the selected mode resumes when it ends.
  if (m.dashboard_active && !m.dashboard_usb) {
    snprintf(buf, sizeof(buf), "Paused, then %s", mode);
  } else {
    snprintf(buf, sizeof(buf), "Mode: %s", mode);
  }
  text_at(c, kListY[0], buf);

  const char *detecting = m.wifi_capture && m.ble_scan ? "Detecting WiFi + BLE"
                          : m.wifi_capture             ? "Detecting WiFi only"
                          : m.ble_scan                 ? "Detecting BLE only"
                                                       : "Not detecting";
  if (!m.dashboard_active) {
    text_at(c, kListY[1], detecting);
    text_at(c, kListY[2], "Dashboard off");
    if (m.dashboard_failed) text_at(c, kListY[3], "Last start failed");
    text_at(c, kListY[5], "Hold: device menu");
    return;
  }
  if (m.dashboard_usb) {
    // Detection keeps running; the page is on the USB-C cable's network.
    text_at(c, kListY[1], detecting);
    text_at(c, kListY[2], "USB dashboard on", true);
    snprintf(buf, sizeof(buf), "http://%s", m.dashboard_address[0] ? m.dashboard_address : "?");
    text_at(c, kListY[3], buf);
    text_at(c, kListY[5], "Hold: device menu");
    return;
  }

  // Wi-Fi session: how to connect. The passkey shows only while it is still
  // the generated one.
  char left[8];
  format_age(m.dashboard_left_ms, left, sizeof(left));
  snprintf(buf, sizeof(buf), "Dashboard %s left", left);
  text_at(c, kListY[1], buf, true);
  snprintf(buf, sizeof(buf), "SSID %s", m.dashboard_ssid);
  text_at(c, kListY[2], buf);
  if (m.dashboard_passkey[0]) {
    snprintf(buf, sizeof(buf), "Key  %s", m.dashboard_passkey);
    text_at(c, kListY[3], buf);
  }
  snprintf(buf, sizeof(buf), "http://%s", m.dashboard_address[0] ? m.dashboard_address : "?");
  text_at(c, kListY[4], buf);
  snprintf(buf, sizeof(buf), "Clients %u", m.dashboard_clients);
  text_at(c, kListY[5], buf);
}

void render(Canvas &c, const UiModel &m, const UiState &s) {
  if (m.erasing) {
    render_erasing(c, m);
    return;
  }
  if (m.power_off_armed) {
    render_power_off(c);
    return;
  }
  if (s.overlay != Overlay::None) {
    render_menu(c, m, s);
  } else {
    switch (s.screen) {
      case Screen::Detections:
        render_detections(c, m);
        break;
      case Screen::Data:
        render_data(c, m);
        break;
      case Screen::Device:
        render_device(c, m);
        break;
      default:
        render_home(c, m);
        break;
    }
  }
  if (message_active(s, m.now_ms)) {
    c.fill_rect(0, 55, kWidth, 9, false);
    text_at(c, 56, s.message, true);
  }
}

}  // namespace fy::ui
