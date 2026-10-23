// SPDX-License-Identifier: MIT
//
// Host unit tests for the dashboard's credentials, login, pages, and
// exports: pio test -e native

#include <unity.h>

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "fy_dashboard_auth.h"
#include "fy_dashboard_export.h"
#include "fy_dashboard_notes.h"
#include "fy_dashboard_text.h"
#include "fy_signatures.h"

using namespace fy::dashboard;
using fy::store::EncounterRecord;
using fy::store::LocationSource;
using fy::store::LocationState;

void setUp() {}
void tearDown() {}

namespace {

// Replays a fixed byte sequence, then counts upwards.
struct Bytes {
  std::vector<uint8_t> seq;
  size_t pos = 0;
  uint8_t counter = 0;
};

uint8_t next_byte(void *ctx) {
  Bytes *b = static_cast<Bytes *>(ctx);
  if (b->pos < b->seq.size()) return b->seq[b->pos++];
  return b->counter++;
}

bool in_alphabet(char c) { return c != '\0' && strchr(kAlphabet, c) != nullptr; }

Status sample_status() {
  Status s = {};
  s.board = "heltec_v43";
  s.uptime_s = 3725;
  s.wifi_session = true;
  s.detection_paused = true;
  s.idle_minutes = 10;
  s.battery_available = true;
  s.battery_valid = true;
  s.battery_mv = 4050;
  s.battery_percent = 87;
  s.gps_available = true;
  s.gps_fix = true;
  s.fix_type = 3;
  s.satellites = 9;
  s.reported = 12;
  s.reported_specific = 2;
  s.storage_available = true;
  s.storage_percent = 4;
  s.records_used = 1234;
  s.records_capacity = 32768;
  s.encounter_records = 1200;
  s.passkey_temporary = true;
  s.record_seq = 1235;
  s.phone_time = true;
  return s;
}

fy::store::Location fix(int32_t lat_e7, int32_t lon_e7, LocationState state, uint8_t hdop_x10) {
  fy::store::Location l = {};
  l.lat_e7 = lat_e7;
  l.lon_e7 = lon_e7;
  l.hdop_x10 = hdop_x10;
  l.age_s = 3;
  l.source_state = static_cast<uint8_t>(static_cast<uint8_t>(LocationSource::Gnss) |
                                        (static_cast<uint8_t>(state) << 4));
  return l;
}

EncounterRecord sample_encounter() {
  EncounterRecord r = {};
  r.h.type = fy::store::kTypeEncounter;
  r.h.seq = 42;
  r.h.boot = 7;
  r.h.flags = fy::store::kFlagSpecific | fy::store::kFlagFinal;
  r.encounter = 5;
  const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, 0x03};
  memcpy(r.mac, mac, sizeof(mac));
  r.count = 3;
  r.rssi_min = -90;
  r.rssi_max = -61;
  r.rule = static_cast<uint8_t>(fy::Rule::WifiProbeIeSignature);
  r.first.uptime_ms = 1000;
  r.first.utc = 1709208000;  // 2024-02-29T12:00:00Z
  r.first.rssi = -90;
  r.first.channel = 6;
  r.first.loc = fix(-123456789, 1234567890, LocationState::Current, 12);
  r.last = r.first;
  r.last.uptime_ms = 9000;
  r.last.utc = 1709208008;
  r.last.loc = fix(0, 0, LocationState::None, 0);
  r.best = r.first;
  r.best.rssi = -61;
  r.best.loc = fix(5, -5, LocationState::Stale, 0);
  return r;
}

Note make_note(uint8_t last_byte, NoteLabel label, bool ignored, const char *text) {
  Note n = {};
  const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, last_byte};
  memcpy(n.mac, mac, sizeof(mac));
  n.label = label;
  n.ignored = ignored;
  strcpy(n.text, text);
  return n;
}

// Fields in one CSV line, honouring quotes.
size_t csv_fields(const char *line) {
  size_t n = 1;
  bool quoted = false;
  for (const char *p = line; *p && *p != '\r'; p++) {
    if (*p == '"') quoted = !quoted;
    if (*p == ',' && !quoted) n++;
  }
  return n;
}

}  // namespace

// ---- Credentials -----------------------------------------------------------------------

void test_alphabet_avoids_confusable_characters() {
  TEST_ASSERT_EQUAL_UINT(31, strlen(kAlphabet));
  for (const char c : {'0', '1', 'i', 'l', 'o'}) TEST_ASSERT_NULL(strchr(kAlphabet, c));
}

void test_symbols_are_uniform_by_rejection() {
  // 248..255 would bias the draw toward the first symbols; they are skipped.
  Bytes b{{255, 248, 0}};
  TEST_ASSERT_EQUAL_INT('a', random_symbol(next_byte, &b));
  TEST_ASSERT_EQUAL_UINT(3, b.pos);
  Bytes c{{30, 31, 247}};
  TEST_ASSERT_EQUAL_INT('9', random_symbol(next_byte, &c));
  TEST_ASSERT_EQUAL_INT('a', random_symbol(next_byte, &c));
  TEST_ASSERT_EQUAL_INT(kAlphabet[247 % 31], random_symbol(next_byte, &c));
}

void test_passkey_format() {
  Bytes b;
  char key[16];
  TEST_ASSERT_TRUE(make_passkey(next_byte, &b, key, sizeof(key)));
  TEST_ASSERT_EQUAL_UINT(14, strlen(key));
  for (int i = 0; i < 14; i++) {
    if (i == 4 || i == 9) {
      TEST_ASSERT_EQUAL_INT('-', key[i]);
    } else {
      TEST_ASSERT_TRUE(in_alphabet(key[i]));
    }
  }
  TEST_ASSERT_EQUAL_STRING("abcd-efgh-jkmn", key);  // counter bytes 0..11
  char small[14];
  TEST_ASSERT_FALSE(make_passkey(next_byte, &b, small, sizeof(small)));
}

void test_ssid_format_and_limits() {
  Bytes b;
  char ssid[33];
  TEST_ASSERT_TRUE(make_ssid("FY-", next_byte, &b, ssid, sizeof(ssid)));
  TEST_ASSERT_EQUAL_STRING("FY-ABCD", ssid);
  // 28-character prefix + 4 = 32, the SSID limit; one more does not fit.
  const char *p28 = "abcdefghijklmnopqrstuvwxyz01";
  TEST_ASSERT_TRUE(make_ssid(p28, next_byte, &b, ssid, sizeof(ssid)));
  TEST_ASSERT_EQUAL_UINT(32, strlen(ssid));
  TEST_ASSERT_FALSE(make_ssid("abcdefghijklmnopqrstuvwxyz012", next_byte, &b, ssid, sizeof(ssid)));
  char small[7];
  TEST_ASSERT_FALSE(make_ssid("FY-", next_byte, &b, small, sizeof(small)));
}

// ---- Pages -----------------------------------------------------------------------------

void test_login_page() {
  static char html[4096];
  const size_t n = render_login_page(LoginNotice::None, 0, html, sizeof(html));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_UINT(strlen(html), n);
  TEST_ASSERT_NOT_NULL(strstr(html, "<form method=\"post\" action=\"/login\">"));
  TEST_ASSERT_NOT_NULL(strstr(html, "name=\"password\" type=\"password\""));
  TEST_ASSERT_NULL(strstr(html, "<script"));
  TEST_ASSERT_NULL(strstr(html, "class=\"err\""));
  render_login_page(LoginNotice::Failed, 0, html, sizeof(html));
  TEST_ASSERT_NOT_NULL(strstr(html, "Wrong password."));
  render_login_page(LoginNotice::Wait, 17, html, sizeof(html));
  TEST_ASSERT_NOT_NULL(strstr(html, "Try again in 17 s."));
  render_login_page(LoginNotice::Expired, 0, html, sizeof(html));
  TEST_ASSERT_NOT_NULL(strstr(html, "session ended"));
}

void test_first_login_page_asks_only_for_the_new_password() {
  static char html[4096];
  TEST_ASSERT_TRUE(render_password_page(true, "", html, sizeof(html)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(html, "Set a new password"));
  TEST_ASSERT_NOT_NULL(strstr(html, "name=\"new\""));
  TEST_ASSERT_NOT_NULL(strstr(html, "name=\"confirm\""));
  TEST_ASSERT_NULL(strstr(html, "name=\"current\""));
  TEST_ASSERT_NULL(strstr(html, "href=\"/\""));  // no way around it
  TEST_ASSERT_NULL(strstr(html, "<script"));
}

void test_change_password_page() {
  static char html[4096];
  TEST_ASSERT_TRUE(render_password_page(false, "Bad <thing> & more", html, sizeof(html)) > 0);
  TEST_ASSERT_NOT_NULL(strstr(html, "Change password"));
  TEST_ASSERT_NOT_NULL(strstr(html, "name=\"current\""));
  TEST_ASSERT_NOT_NULL(strstr(html, "name=\"new\""));
  TEST_ASSERT_NOT_NULL(strstr(html, "Bad &lt;thing&gt; &amp; more"));
  TEST_ASSERT_NOT_NULL(strstr(html, "href=\"/\""));
}

void test_pages_too_small() {
  char html[256];
  TEST_ASSERT_EQUAL_UINT(0, render_login_page(LoginNotice::None, 0, html, sizeof(html)));
  TEST_ASSERT_EQUAL_STRING("", html);
  TEST_ASSERT_EQUAL_UINT(0, render_password_page(true, "", html, sizeof(html)));
}

void test_status_json() {
  static char json[1024];
  const size_t n = format_status_json(sample_status(), json, sizeof(json));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_UINT(strlen(json), n);
  TEST_ASSERT_EQUAL_INT('{', json[0]);
  TEST_ASSERT_EQUAL_INT('}', json[n - 1]);
  TEST_ASSERT_NOT_NULL(strstr(json, "\"board\":\"heltec_v43\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"dashboard\":\"wifi\",\"detection_paused\":true"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"battery\":{\"mv\":4050,\"percent\":87}"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"gps\":{\"fix\":true,\"fix_type\":3,\"satellites\":9}"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"matches\":{\"reported\":12,\"specific\":2}"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"used\":1234,\"capacity\":32768"));
  TEST_ASSERT_NOT_NULL(strstr(
      json, "\"record_seq\":1235,\"phone\":{\"time\":true}}"));

  Status s = sample_status();
  s.battery_valid = false;
  s.gps_available = false;
  s.storage_available = false;
  s.wifi_session = false;
  s.phone_time = false;
  format_status_json(s, json, sizeof(json));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"phone\":{\"time\":false}"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"battery\":null"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"gps\":null"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"storage\":null"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"dashboard\":\"usb\""));
  char small[32];
  TEST_ASSERT_EQUAL_UINT(0, format_status_json(s, small, sizeof(small)));
}

// ---- Login -------------------------------------------------------------------------------

void test_form_value_decoding() {
  const char *body = "pass=no&password=a%20b+c%21&x=";
  char v[32];
  TEST_ASSERT_TRUE(form_value(body, strlen(body), "password", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("a b c!", v);
  TEST_ASSERT_TRUE(form_value(body, strlen(body), "pass", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("no", v);
  TEST_ASSERT_TRUE(form_value(body, strlen(body), "x", v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("", v);
  TEST_ASSERT_FALSE(form_value(body, strlen(body), "missing", v, sizeof(v)));
  // Only the given length is read.
  TEST_ASSERT_FALSE(form_value(body, 7, "password", v, sizeof(v)));
}

void test_form_value_rejects_bad_input() {
  char v[8];
  const char *bad_hex = "p=%zz";
  TEST_ASSERT_FALSE(form_value(bad_hex, strlen(bad_hex), "p", v, sizeof(v)));
  const char *truncated = "p=ab%4";
  TEST_ASSERT_FALSE(form_value(truncated, strlen(truncated), "p", v, sizeof(v)));
  const char *nul = "p=a%00b";
  TEST_ASSERT_FALSE(form_value(nul, strlen(nul), "p", v, sizeof(v)));
  const char *long_value = "p=12345678";
  TEST_ASSERT_FALSE(form_value(long_value, strlen(long_value), "p", v, sizeof(v)));
  const char *fits = "p=1234567";
  TEST_ASSERT_TRUE(form_value(fits, strlen(fits), "p", v, sizeof(v)));
}

void test_new_password_rules() {
  const char *def = "changeme";
  TEST_ASSERT_EQUAL(PasswordProblem::Missing, check_new_password("", "", def));
  TEST_ASSERT_EQUAL(PasswordProblem::Missing, check_new_password("goodpass1", "", def));
  TEST_ASSERT_EQUAL(PasswordProblem::TooShort, check_new_password("short", "short", def));
  const std::string long_pw(65, 'x');
  TEST_ASSERT_EQUAL(PasswordProblem::TooLong, check_new_password(long_pw.c_str(), long_pw.c_str(), def));
  const std::string max_pw(64, 'x');
  TEST_ASSERT_EQUAL(PasswordProblem::None, check_new_password(max_pw.c_str(), max_pw.c_str(), def));
  TEST_ASSERT_EQUAL(PasswordProblem::BadCharacter, check_new_password("tab\there!", "tab\there!", def));
  TEST_ASSERT_EQUAL(PasswordProblem::Mismatch, check_new_password("goodpass1", "goodpass2", def));
  TEST_ASSERT_EQUAL(PasswordProblem::SameAsDefault, check_new_password(def, def, def));
  TEST_ASSERT_EQUAL(PasswordProblem::None, check_new_password("a long pass phrase", "a long pass phrase", def));
  TEST_ASSERT_EQUAL(PasswordProblem::None, check_new_password("caf\xc3\xa9-caf\xc3\xa9", "caf\xc3\xa9-caf\xc3\xa9", def));
  TEST_ASSERT_EQUAL_STRING("", describe(PasswordProblem::None));
  TEST_ASSERT_TRUE(strlen(describe(PasswordProblem::Mismatch)) > 0);
  TEST_ASSERT_TRUE(strlen(describe(PasswordProblem::WrongCurrent)) > 0);
}

void test_constant_time_equal() {
  TEST_ASSERT_TRUE(constant_time_equal("abcdef", "abcdef", 6));
  TEST_ASSERT_FALSE(constant_time_equal("abcdef", "abcdeg", 6));
  TEST_ASSERT_TRUE(constant_time_equal("x", "y", 0));
}

void test_sessions_create_touch_end() {
  Sessions s;
  Bytes b;
  char token[kTokenLen + 1];
  s.create(next_byte, &b, 1000, token);
  TEST_ASSERT_EQUAL_UINT(kTokenLen, strlen(token));
  TEST_ASSERT_EQUAL_STRING("000102030405060708090a0b0c0d0e0f", token);
  TEST_ASSERT_TRUE(s.touch(token, 2000));
  TEST_ASSERT_FALSE(s.touch("000102030405060708090a0b0c0d0e0e", 2000));
  TEST_ASSERT_FALSE(s.touch("0001020304", 2000));  // wrong length
  TEST_ASSERT_FALSE(s.touch("", 2000));
  TEST_ASSERT_FALSE(s.touch(nullptr, 2000));
  s.end(token);
  TEST_ASSERT_FALSE(s.touch(token, 2000));
}

void test_sessions_expire() {
  Sessions s;
  Bytes b;
  char token[kTokenLen + 1];
  s.create(next_byte, &b, 0, token);
  TEST_ASSERT_TRUE(s.touch(token, Sessions::kIdleMs));  // exactly at the limit
  TEST_ASSERT_FALSE(s.touch(token, 2 * Sessions::kIdleMs + 1));  // idle too long

  // Activity keeps a session alive only up to its lifetime.
  s.create(next_byte, &b, 0, token);
  uint32_t t = 0;
  for (; t + Sessions::kIdleMs / 2 <= Sessions::kLifetimeMs; t += Sessions::kIdleMs / 2) {
    TEST_ASSERT_TRUE(s.touch(token, t));
  }
  TEST_ASSERT_FALSE(s.touch(token, Sessions::kLifetimeMs + 1));

  // Background requests check a session without keeping it alive.
  s.create(next_byte, &b, 0, token);
  TEST_ASSERT_TRUE(s.touch(token, Sessions::kIdleMs / 2, false));
  TEST_ASSERT_TRUE(s.touch(token, Sessions::kIdleMs, false));
  TEST_ASSERT_FALSE(s.touch(token, Sessions::kIdleMs + 1, false));

  // Uptime wrapping around does not end a session early.
  s.create(next_byte, &b, UINT32_MAX - 1000, token);
  TEST_ASSERT_TRUE(s.touch(token, 5000));
}

void test_sessions_replace_least_recently_used() {
  Sessions s;
  Bytes b;
  char tokens[Sessions::kMax + 1][kTokenLen + 1];
  for (size_t i = 0; i < Sessions::kMax; i++) s.create(next_byte, &b, 100 * i, tokens[i]);
  TEST_ASSERT_TRUE(s.touch(tokens[0], 1000));  // now the most recently used
  s.create(next_byte, &b, 1100, tokens[Sessions::kMax]);
  TEST_ASSERT_TRUE(s.touch(tokens[0], 1200));
  TEST_ASSERT_FALSE(s.touch(tokens[1], 1200));  // least recently used, replaced
  TEST_ASSERT_TRUE(s.touch(tokens[Sessions::kMax], 1200));
  s.end_all();
  for (auto &t : tokens) TEST_ASSERT_FALSE(s.touch(t, 1300));
}

void test_login_throttle() {
  LoginThrottle t;
  for (uint32_t i = 0; i < LoginThrottle::kFreeAttempts; i++) {
    TEST_ASSERT_EQUAL_UINT32(0, t.wait_ms(1000));
    t.failed(1000);
  }
  TEST_ASSERT_EQUAL_UINT32(LoginThrottle::kBaseDelayMs, t.wait_ms(1000));
  TEST_ASSERT_EQUAL_UINT32(1000, t.wait_ms(1000 + LoginThrottle::kBaseDelayMs - 1000));
  TEST_ASSERT_EQUAL_UINT32(0, t.wait_ms(1000 + LoginThrottle::kBaseDelayMs));
  t.failed(10000);
  TEST_ASSERT_EQUAL_UINT32(2 * LoginThrottle::kBaseDelayMs, t.wait_ms(10000));
  for (int i = 0; i < 20; i++) t.failed(20000);
  TEST_ASSERT_EQUAL_UINT32(LoginThrottle::kMaxDelayMs, t.wait_ms(20000));
  t.succeeded();
  TEST_ASSERT_EQUAL_UINT32(0, t.wait_ms(20000));
}

// ---- Exports -------------------------------------------------------------------------------

void test_format_utc() {
  char out[21];
  format_utc(0, out);
  TEST_ASSERT_EQUAL_STRING("", out);
  format_utc(946684800, out);
  TEST_ASSERT_EQUAL_STRING("2000-01-01T00:00:00Z", out);
  format_utc(1709208000, out);
  TEST_ASSERT_EQUAL_STRING("2024-02-29T12:00:00Z", out);
  format_utc(1735689599, out);
  TEST_ASSERT_EQUAL_STRING("2024-12-31T23:59:59Z", out);
  format_utc(UINT32_MAX, out);
  TEST_ASSERT_EQUAL_STRING("2106-02-07T06:28:15Z", out);
}

void test_encounter_json() {
  static char json[kEncounterTextCap];
  const EncounterRecord r = sample_encounter();
  const size_t n = encounter_json(r, nullptr, json, sizeof(json));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_UINT(strlen(json), n);
  TEST_ASSERT_NOT_NULL(strstr(json, "{\"seq\":42,\"boot\":7,\"encounter\":5,\"mac\":\"b4:1e:52:01:02:03\""));
  TEST_ASSERT_NOT_NULL(strstr(
      json, "\"rule\":\"wifi_probe_ie_sig\",\"type\":\"Flock camera\",\"vendor\":\"Flock Safety\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"specific\":true,\"transport\":\"wifi\",\"closed\":true"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"sightings\":3,\"rssi_min\":-90,\"rssi_max\":-61"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"first\":{\"uptime_ms\":1000,\"utc\":\"2024-02-29T12:00:00Z\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"loc\":{\"lat\":-12.3456789,\"lon\":123.4567890,\"hdop\":1.2,"
                                    "\"age_s\":3,\"state\":\"current\",\"source\":\"gnss\"}"));
  // No fix: no coordinates at all, not zeros.
  TEST_ASSERT_NOT_NULL(strstr(json, "\"last\":{\"uptime_ms\":9000,\"utc\":\"2024-02-29T12:00:08Z\","
                                    "\"rssi\":-90,\"channel\":6,\"loc\":null}"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"lat\":0.0000005,\"lon\":-0.0000005,\"hdop\":null"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"state\":\"stale\""));
  TEST_ASSERT_NOT_NULL(strstr(json, ",\"label\":null,\"ignored\":false,\"note\":null}"));
  TEST_ASSERT_EQUAL_INT('}', json[n - 1]);
}

void test_encounter_json_with_note() {
  static char json[kEncounterTextCap];
  const EncounterRecord r = sample_encounter();
  const Note note = make_note(0x03, NoteLabel::Confirmed, true, "say \"hi\"");
  encounter_json(r, &note, json, sizeof(json));
  TEST_ASSERT_NOT_NULL(
      strstr(json, ",\"label\":\"confirmed\",\"ignored\":true,\"note\":\"say \\\"hi\\\"\"}"));
}

void test_encounter_json_escapes_names_and_unknown_rules() {
  static char json[kEncounterTextCap];
  EncounterRecord r = sample_encounter();
  r.rule = 200;  // from newer firmware
  r.h.flags = fy::store::kFlagBle;
  r.first.utc = 0;
  strcpy(r.name, "a\"b\\c\x01\xe9");
  encounter_json(r, nullptr, json, sizeof(json));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"rule\":\"unknown\",\"type\":\"Unknown\",\"vendor\":null"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"specific\":false,\"transport\":\"ble\",\"closed\":false"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"name\":\"a\\\"b\\\\c\\u0001\\u00e9\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"utc\":null"));
}

void test_encounter_text_fits_its_buffer() {
  // The longest name in its most expanded form, every position present.
  EncounterRecord r = sample_encounter();
  memset(r.name, 0xff, fy::store::kNameLen - 1);
  r.name[fy::store::kNameLen - 1] = '\0';
  r.best.loc = r.first.loc;
  r.last.loc = r.first.loc;
  r.count = 65535;
  r.h.seq = r.h.boot = r.encounter = UINT32_MAX;
  r.first.uptime_ms = r.last.uptime_ms = r.best.uptime_ms = UINT32_MAX;
  // A note of nothing but quotes doubles in both formats.
  Note note = make_note(0x03, NoteLabel::FalsePositive, true, "");
  memset(note.text, '"', kNoteTextCap - 1);
  note.text[kNoteTextCap - 1] = '\0';
  static char text[kEncounterTextCap];
  TEST_ASSERT_TRUE(encounter_json(r, &note, text, sizeof(text)) > 0);
  TEST_ASSERT_TRUE(encounter_csv(r, &note, text, sizeof(text)) > 0);
}

void test_encounter_csv() {
  static char line[kEncounterTextCap];
  EncounterRecord r = sample_encounter();
  const size_t n = encounter_csv(r, nullptr, line, sizeof(line));
  TEST_ASSERT_TRUE(n > 2);
  TEST_ASSERT_EQUAL_STRING("\r\n", line + n - 2);
  TEST_ASSERT_EQUAL_UINT(csv_fields(encounter_csv_header()), csv_fields(line));
  TEST_ASSERT_EQUAL_UINT(36, csv_fields(line));
  TEST_ASSERT_NOT_NULL(strstr(
      line, "42,7,5,b4:1e:52:01:02:03,Flock camera,Flock Safety,wifi_probe_ie_sig,specific,wifi,"
            "yes,3,-90,-61,"));
  TEST_ASSERT_NOT_NULL(strstr(line, ",2024-02-29T12:00:00Z,2024-02-29T12:00:08Z,2024-02-29T12:00:00Z,-61,6,"));
  // best: stale, no HDOP; first: current; last: no position; no note.
  TEST_ASSERT_NOT_NULL(strstr(
      line, ",0.0000005,-0.0000005,stale,gnss,,3,-12.3456789,123.4567890,current,,,,1000,9000,,no,\r\n"));

  // Names that would be formulas or break the row are neutralised.
  strcpy(r.name, "=HYPERLINK(\"x\",1)");
  encounter_csv(r, nullptr, line, sizeof(line));
  TEST_ASSERT_NOT_NULL(strstr(line, ",\"'=HYPERLINK(\"\"x\"\",1)\","));
  TEST_ASSERT_EQUAL_UINT(36, csv_fields(line));
  strcpy(r.name, "-1\nx");
  encounter_csv(r, nullptr, line, sizeof(line));
  TEST_ASSERT_NOT_NULL(strstr(line, ",\"'-1?x\","));

  // A note, which is neutralised like a name.
  const Note note = make_note(0x03, NoteLabel::FalsePositive, true, "=a,b");
  encounter_csv(r, &note, line, sizeof(line));
  TEST_ASSERT_NOT_NULL(strstr(line, ",1000,9000,false_positive,yes,\"'=a,b\"\r\n"));
  TEST_ASSERT_EQUAL_UINT(36, csv_fields(line));
}

// ---- Notes -------------------------------------------------------------------------------

void test_parse_mac() {
  uint8_t mac[6];
  TEST_ASSERT_TRUE(parse_mac("B4:1e:52:0a:Ff:03", mac));
  const uint8_t want[6] = {0xb4, 0x1e, 0x52, 0x0a, 0xff, 0x03};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, mac, 6);
  for (const char *bad : {"", "b4:1e:52:0a:ff", "b4:1e:52:0a:ff:03:", "b4-1e-52-0a-ff-03",
                          "b4:1e:52:0a:fg:03", " b4:1e:52:0a:ff:0"}) {
    TEST_ASSERT_FALSE_MESSAGE(parse_mac(bad, mac), bad);
  }
  TEST_ASSERT_FALSE(parse_mac(nullptr, mac));
}

void test_note_labels() {
  NoteLabel l = NoteLabel::Confirmed;
  TEST_ASSERT_TRUE(parse_label("", &l));
  TEST_ASSERT_EQUAL(NoteLabel::None, l);
  TEST_ASSERT_TRUE(parse_label("false_positive", &l));
  TEST_ASSERT_EQUAL(NoteLabel::FalsePositive, l);
  TEST_ASSERT_TRUE(parse_label(label_name(NoteLabel::Confirmed), &l));
  TEST_ASSERT_EQUAL(NoteLabel::Confirmed, l);
  TEST_ASSERT_FALSE(parse_label("Confirmed", &l));
  TEST_ASSERT_FALSE(parse_label(nullptr, &l));
}

void test_clean_note_text() {
  char out[kNoteTextCap];
  clean_note_text("  pole by the \x01gate\xe9  ", out);
  TEST_ASSERT_EQUAL_STRING("pole by the gate", out);
  clean_note_text(nullptr, out);
  TEST_ASSERT_EQUAL_STRING("", out);
  std::string long_text(200, 'x');
  clean_note_text(long_text.c_str(), out);
  TEST_ASSERT_EQUAL_UINT(kNoteTextCap - 1, strlen(out));
}

void test_notes_set_find_remove() {
  static Notes notes;
  notes.clear();
  const Note a = make_note(1, NoteLabel::Confirmed, false, "");
  TEST_ASSERT_TRUE(notes.set(a));
  TEST_ASSERT_TRUE(notes.set(make_note(2, NoteLabel::None, true, "")));
  TEST_ASSERT_EQUAL_UINT(2, notes.count());
  TEST_ASSERT_EQUAL(NoteLabel::Confirmed, notes.find(a.mac)->label);

  // Replacing keeps one entry per address; an empty note removes it.
  TEST_ASSERT_TRUE(notes.set(make_note(1, NoteLabel::FalsePositive, false, "x")));
  TEST_ASSERT_EQUAL_UINT(2, notes.count());
  TEST_ASSERT_EQUAL_STRING("x", notes.find(a.mac)->text);
  TEST_ASSERT_TRUE(notes.set(make_note(1, NoteLabel::None, false, "")));
  TEST_ASSERT_EQUAL_UINT(1, notes.count());
  TEST_ASSERT_NULL(notes.find(a.mac));
  TEST_ASSERT_TRUE(notes.set(make_note(9, NoteLabel::None, false, "")));  // nothing to remove
  TEST_ASSERT_EQUAL_UINT(1, notes.count());

  for (uint8_t i = 10; notes.count() < Notes::kMax; i++) {
    TEST_ASSERT_TRUE(notes.set(make_note(i, NoteLabel::None, true, "")));
  }
  TEST_ASSERT_FALSE(notes.set(make_note(250, NoteLabel::None, true, "")));
  TEST_ASSERT_TRUE(notes.set(make_note(2, NoteLabel::Confirmed, true, "")));  // existing: fine
}

void test_notes_serialize_round_trip() {
  static Notes notes, back;
  notes.clear();
  notes.set(make_note(1, NoteLabel::Confirmed, false, "north pole"));
  notes.set(make_note(2, NoteLabel::FalsePositive, true, ""));
  static uint8_t buf[Notes::kMaxBytes];
  const size_t len = notes.serialize(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(1 + 2 * Notes::kEntryBytes, len);
  TEST_ASSERT_EQUAL_UINT(0, notes.serialize(buf, len - 1));
  TEST_ASSERT_TRUE(back.deserialize(buf, len));
  TEST_ASSERT_EQUAL_UINT(2, back.count());
  TEST_ASSERT_EQUAL_STRING("north pole", back.find(notes.at(0).mac)->text);
  TEST_ASSERT_TRUE(back.find(notes.at(1).mac)->ignored);

  // Malformed data leaves the table empty.
  TEST_ASSERT_FALSE(back.deserialize(buf, len - 1));
  TEST_ASSERT_EQUAL_UINT(0, back.count());
  buf[0] = 99;
  TEST_ASSERT_FALSE(back.deserialize(buf, len));
  buf[0] = 1;
  buf[1 + 6] = 7;  // no such label
  TEST_ASSERT_FALSE(back.deserialize(buf, len));
  TEST_ASSERT_EQUAL_UINT(0, back.count());
  TEST_ASSERT_TRUE(back.deserialize(buf, 1));  // a stored empty table
}

void test_notes_json() {
  static Notes notes;
  notes.clear();
  char json[256];
  TEST_ASSERT_EQUAL_UINT(2, notes_json(notes, json, sizeof(json)));
  TEST_ASSERT_EQUAL_STRING("[]", json);
  notes.set(make_note(1, NoteLabel::Confirmed, false, "a\"b"));
  notes.set(make_note(2, NoteLabel::None, true, ""));
  TEST_ASSERT_TRUE(notes_json(notes, json, sizeof(json)) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "[{\"mac\":\"b4:1e:52:01:02:01\",\"label\":\"confirmed\",\"ignored\":false,\"note\":\"a\\\"b\"},"
      "{\"mac\":\"b4:1e:52:01:02:02\",\"label\":\"\",\"ignored\":true,\"note\":\"\"}]",
      json);
  TEST_ASSERT_EQUAL_UINT(0, notes_json(notes, json, 40));

  // A note at its longest fits the buffer the dashboard streams it from.
  Note n = make_note(0xff, NoteLabel::FalsePositive, false, "");
  memset(n.text, '"', kNoteTextCap - 1);
  char one[kNoteJsonCap];
  const size_t len = note_json(n, one, sizeof(one));
  TEST_ASSERT_TRUE(len > 2 * (kNoteTextCap - 1));
  TEST_ASSERT_EQUAL_UINT(0, note_json(n, one, len));  // no room for the NUL
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_alphabet_avoids_confusable_characters);
  RUN_TEST(test_symbols_are_uniform_by_rejection);
  RUN_TEST(test_passkey_format);
  RUN_TEST(test_ssid_format_and_limits);
  RUN_TEST(test_login_page);
  RUN_TEST(test_first_login_page_asks_only_for_the_new_password);
  RUN_TEST(test_change_password_page);
  RUN_TEST(test_pages_too_small);
  RUN_TEST(test_status_json);
  RUN_TEST(test_form_value_decoding);
  RUN_TEST(test_form_value_rejects_bad_input);
  RUN_TEST(test_new_password_rules);
  RUN_TEST(test_constant_time_equal);
  RUN_TEST(test_sessions_create_touch_end);
  RUN_TEST(test_sessions_expire);
  RUN_TEST(test_sessions_replace_least_recently_used);
  RUN_TEST(test_login_throttle);
  RUN_TEST(test_format_utc);
  RUN_TEST(test_encounter_json);
  RUN_TEST(test_encounter_json_with_note);
  RUN_TEST(test_encounter_json_escapes_names_and_unknown_rules);
  RUN_TEST(test_encounter_text_fits_its_buffer);
  RUN_TEST(test_encounter_csv);
  RUN_TEST(test_parse_mac);
  RUN_TEST(test_note_labels);
  RUN_TEST(test_clean_note_text);
  RUN_TEST(test_notes_set_find_remove);
  RUN_TEST(test_notes_serialize_round_trip);
  RUN_TEST(test_notes_json);
  return UNITY_END();
}
