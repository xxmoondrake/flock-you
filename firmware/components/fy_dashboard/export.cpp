// SPDX-License-Identifier: MIT

#include "fy_dashboard_export.h"

#include "fy_signatures.h"
#include "writer.h"

namespace fy::dashboard {
namespace {

using store::EncounterRecord;
using store::Location;
using store::LocationSource;
using store::LocationState;
using store::Observation;

struct RuleText {
  const char *id;
  const char *label;
  const char *vendor;  // null when the rule names none
};

// Records from newer firmware may carry rules this build does not know.
RuleText rule_text(uint8_t rule, const uint8_t *mac) {
  if (rule >= static_cast<uint8_t>(Rule::Count)) return {"unknown", "Unknown", nullptr};
  const Rule r = static_cast<Rule>(rule);
  const Classification c = classify(r, mac);
  return {rule_info(r).id, device_type_info(c.type).label, c.vendor};
}

bool has_position(const Location &loc) {
  return loc.state() == LocationState::Current || loc.state() == LocationState::Stale;
}

const char *state_name(const Location &loc) {
  switch (loc.state()) {
    case LocationState::Current: return "current";
    case LocationState::Stale: return "stale";
    default: return "";
  }
}

const char *source_name(const Location &loc) {
  switch (loc.source()) {
    case LocationSource::Gnss: return "gnss";
    case LocationSource::Phone: return "phone";
    default: return "unknown";
  }
}

void add_degrees(Writer &w, int32_t e7) {
  const bool negative = e7 < 0;
  const uint32_t a = static_cast<uint32_t>(negative ? -static_cast<int64_t>(e7) : e7);
  w.add("%s%lu.%07lu", negative ? "-" : "", static_cast<unsigned long>(a / 10000000u),
        static_cast<unsigned long>(a % 10000000u));
}

void add_mac(Writer &w, const uint8_t mac[6]) {
  w.add("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void add_observation_json(Writer &w, const Observation &o) {
  char utc[21];
  format_utc(o.utc, utc);
  w.add("{\"uptime_ms\":%lu,\"utc\":", static_cast<unsigned long>(o.uptime_ms));
  if (utc[0]) {
    w.add("\"%s\"", utc);
  } else {
    w.add("null");
  }
  w.add(",\"rssi\":%d,\"channel\":%u,\"loc\":", o.rssi, static_cast<unsigned>(o.channel));
  const Location loc = o.loc;  // packed: copy before calling members
  if (!has_position(loc)) {
    w.add("null}");
    return;
  }
  w.add("{\"lat\":");
  add_degrees(w, loc.lat_e7);
  w.add(",\"lon\":");
  add_degrees(w, loc.lon_e7);
  if (loc.hdop_x10) {
    w.add(",\"hdop\":%u.%u", loc.hdop_x10 / 10u, loc.hdop_x10 % 10u);
  } else {
    w.add(",\"hdop\":null");
  }
  w.add(",\"age_s\":%u,\"state\":\"%s\",\"source\":\"%s\"}}", static_cast<unsigned>(loc.age_s),
        state_name(loc), source_name(loc));
}

// lat,lon,fix (three fields, empty without a position).
void add_position_csv(Writer &w, const Observation &o) {
  const Location loc = o.loc;
  if (has_position(loc)) {
    add_degrees(w, loc.lat_e7);
    w.add_char(',');
    add_degrees(w, loc.lon_e7);
    w.add(",%s", state_name(loc));
  } else {
    w.add(",,");
  }
}

}  // namespace

void format_utc(uint32_t unix_s, char out[21]) {
  out[0] = '\0';
  if (unix_s == 0) return;
  // Civil date from days since 1970-01-01 (Howard Hinnant's method).
  const int64_t days = unix_s / 86400;
  const uint32_t secs = unix_s % 86400;
  const int64_t z = days + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  snprintf(out, 21, "%04d-%02d-%02dT%02u:%02u:%02uZ", static_cast<int>(year),
           static_cast<int>(month), static_cast<int>(day), static_cast<unsigned>(secs / 3600),
           static_cast<unsigned>(secs / 60 % 60), static_cast<unsigned>(secs % 60));
}

size_t encounter_json(const EncounterRecord &rec, const Note *note, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  const RuleText rule = rule_text(rec.rule, rec.mac);
  const uint8_t flags = rec.h.flags;
  w.add("{\"seq\":%lu,\"boot\":%lu,\"encounter\":%lu,\"mac\":\"",
        static_cast<unsigned long>(rec.h.seq), static_cast<unsigned long>(rec.h.boot),
        static_cast<unsigned long>(rec.encounter));
  add_mac(w, rec.mac);
  w.add("\",\"rule\":\"%s\",\"type\":", rule.id);
  w.add_json_string(rule.label);
  w.add(",\"vendor\":");
  if (rule.vendor) {
    w.add_json_string(rule.vendor);
  } else {
    w.add("null");
  }
  w.add(
      ",\"specific\":%s,\"transport\":\"%s\",\"closed\":%s,\"sightings\":%u,\"rssi_min\":%d,"
      "\"rssi_max\":%d,\"name\":",
      (flags & store::kFlagSpecific) ? "true" : "false",
      (flags & store::kFlagBle) ? "ble" : "wifi", (flags & store::kFlagFinal) ? "true" : "false",
      static_cast<unsigned>(rec.count), rec.rssi_min, rec.rssi_max);
  w.add_json_string(rec.name, store::kNameLen);
  w.add(",\"first\":");
  add_observation_json(w, rec.first);
  w.add(",\"last\":");
  add_observation_json(w, rec.last);
  w.add(",\"best\":");
  add_observation_json(w, rec.best);
  if (note && note->label != NoteLabel::None) {
    w.add(",\"label\":\"%s\"", label_name(note->label));
  } else {
    w.add(",\"label\":null");
  }
  w.add(",\"ignored\":%s,\"note\":", note && note->ignored ? "true" : "false");
  if (note && note->text[0]) {
    w.add_json_string(note->text, kNoteTextCap);
  } else {
    w.add("null");
  }
  w.add("}");
  return w.finish();
}

const char *encounter_csv_header() {
  return "seq,boot,encounter,mac,type,vendor,rule,strength,transport,closed,sightings,rssi_min,"
         "rssi_max,name,first_utc,last_utc,best_utc,best_rssi,best_channel,best_lat,best_lon,"
         "best_fix,best_source,best_hdop,best_fix_age_s,first_lat,first_lon,"
         "first_fix,last_lat,last_lon,last_fix,first_uptime_ms,last_uptime_ms,label,ignored,"
         "note\r\n";
}

size_t encounter_csv(const EncounterRecord &rec, const Note *note, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  Writer w{out, cap};
  const RuleText rule = rule_text(rec.rule, rec.mac);
  const uint8_t flags = rec.h.flags;
  w.add("%lu,%lu,%lu,", static_cast<unsigned long>(rec.h.seq),
        static_cast<unsigned long>(rec.h.boot), static_cast<unsigned long>(rec.encounter));
  add_mac(w, rec.mac);
  w.add_char(',');
  w.add_csv(rule.label);
  w.add_char(',');
  w.add_csv(rule.vendor ? rule.vendor : "");
  w.add(",%s,%s,%s,%s,%u,%d,%d,", rule.id, (flags & store::kFlagSpecific) ? "specific" : "possible",
        (flags & store::kFlagBle) ? "ble" : "wifi", (flags & store::kFlagFinal) ? "yes" : "no",
        static_cast<unsigned>(rec.count), rec.rssi_min, rec.rssi_max);
  w.add_csv(rec.name, store::kNameLen);
  char first[21], last[21], best[21];
  format_utc(rec.first.utc, first);
  format_utc(rec.last.utc, last);
  format_utc(rec.best.utc, best);
  w.add(",%s,%s,%s,%d,%u,", first, last, best, rec.best.rssi,
        static_cast<unsigned>(rec.best.channel));
  const Location best_loc = rec.best.loc;
  add_position_csv(w, rec.best);
  if (has_position(best_loc)) {
    w.add(",%s,", source_name(best_loc));
    if (best_loc.hdop_x10) w.add("%u.%u", best_loc.hdop_x10 / 10u, best_loc.hdop_x10 % 10u);
    w.add(",%u,", static_cast<unsigned>(best_loc.age_s));
  } else {
    w.add(",,,,");
  }
  add_position_csv(w, rec.first);
  w.add_char(',');
  add_position_csv(w, rec.last);
  w.add(",%lu,%lu,%s,%s,", static_cast<unsigned long>(rec.first.uptime_ms),
        static_cast<unsigned long>(rec.last.uptime_ms),
        note ? label_name(note->label) : "", note && note->ignored ? "yes" : "no");
  w.add_csv(note ? note->text : "", kNoteTextCap);
  w.add("\r\n");
  return w.finish();
}

}  // namespace fy::dashboard
