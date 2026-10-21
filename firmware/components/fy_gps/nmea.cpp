// SPDX-License-Identifier: MIT

#include "fy_nmea.h"

#include <cstdlib>
#include <cstring>

namespace fy::gps {
namespace {

constexpr int kMaxFields = 24;

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Accepts only plain decimal numbers ("-12.5"), never strtod extras such as
// "inf", hex, or exponents.
bool parse_decimal(const char *s, double *out) {
  if (!s || !*s) return false;
  const char *p = s;
  if (*p == '-' || *p == '+') p++;
  bool digits = false;
  bool dot = false;
  for (; *p; p++) {
    if (is_digit(*p)) {
      digits = true;
    } else if (*p == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  *out = strtod(s, nullptr);
  return true;
}

bool parse_uint(const char *s, unsigned *out) {
  if (!s || !*s) return false;
  unsigned v = 0;
  for (const char *p = s; *p; p++) {
    if (!is_digit(*p)) return false;
    v = v * 10 + static_cast<unsigned>(*p - '0');
    if (v > 100000) return false;
  }
  *out = v;
  return true;
}

int two_digits(const char *s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

// "hhmmss" or "hhmmss.sss".
bool parse_time(const char *s, UtcTime *t) {
  if (!s || strlen(s) < 6) return false;
  for (int i = 0; i < 6; i++) {
    if (!is_digit(s[i])) return false;
  }
  const int h = two_digits(s), m = two_digits(s + 2), sec = two_digits(s + 4);
  if (h > 23 || m > 59 || sec > 60) return false;
  int ms = 0;
  if (s[6] == '.') {
    int scale = 100;
    for (const char *p = s + 7; *p; p++) {
      if (!is_digit(*p)) return false;
      ms += (*p - '0') * scale;
      scale /= 10;
    }
  } else if (s[6] != '\0') {
    return false;
  }
  t->hour = static_cast<uint8_t>(h);
  t->minute = static_cast<uint8_t>(m);
  t->second = static_cast<uint8_t>(sec);
  t->millisecond = static_cast<uint16_t>(ms);
  return true;
}

// "ddmmyy". Two-digit years 80-99 are 1980-1999, otherwise 2000-2079.
bool parse_date(const char *s, UtcTime *t) {
  if (!s || strlen(s) != 6) return false;
  for (int i = 0; i < 6; i++) {
    if (!is_digit(s[i])) return false;
  }
  const int d = two_digits(s), mo = two_digits(s + 2), yy = two_digits(s + 4);
  if (d < 1 || d > 31 || mo < 1 || mo > 12) return false;
  t->day = static_cast<uint8_t>(d);
  t->month = static_cast<uint8_t>(mo);
  t->year = static_cast<uint16_t>(yy >= 80 ? 1900 + yy : 2000 + yy);
  return true;
}

// NMEA coordinates are (d)ddmm.mmmm plus a hemisphere letter.
bool parse_coordinate(const char *value, const char *hemi, bool latitude, double *out) {
  double raw;
  if (!parse_decimal(value, &raw) || raw < 0 || !hemi || hemi[1] != '\0') return false;
  const double degrees = static_cast<double>(static_cast<long>(raw / 100));
  const double minutes = raw - degrees * 100;
  if (minutes >= 60) return false;
  double deg = degrees + minutes / 60.0;
  const double limit = latitude ? 90.0 : 180.0;
  if (deg > limit) return false;
  const char pos = latitude ? 'N' : 'E';
  const char neg = latitude ? 'S' : 'W';
  if (hemi[0] == neg) {
    deg = -deg;
  } else if (hemi[0] != pos) {
    return false;
  }
  *out = deg;
  return true;
}

const char *field(char **f, int n, int i) { return i < n ? f[i] : ""; }

}  // namespace

uint8_t NmeaParser::feed(char c) {
  if (c == '$') {
    in_sentence_ = true;
    len_ = 0;
    line_[len_++] = c;
    return 0;
  }
  if (!in_sentence_) return 0;
  if (c == '\r' || c == '\n') {
    in_sentence_ = false;
    line_[len_] = '\0';
    return parse_sentence(line_);
  }
  if (len_ >= kMaxSentence) {
    overflows_++;
    in_sentence_ = false;  // discard until the next '$'
    return 0;
  }
  line_[len_++] = c;
  return 0;
}

uint8_t NmeaParser::parse_sentence(const char *sentence) {
  if (!sentence || sentence[0] != '$') return 0;
  const char *star = strchr(sentence, '*');
  if (!star || hex_value(star[1]) < 0 || hex_value(star[2]) < 0 || star[3] != '\0') {
    checksum_errors_++;
    return 0;
  }
  uint8_t sum = 0;
  for (const char *p = sentence + 1; p < star; p++) sum ^= static_cast<uint8_t>(*p);
  if (sum != static_cast<uint8_t>(hex_value(star[1]) * 16 + hex_value(star[2]))) {
    checksum_errors_++;
    return 0;
  }

  const size_t body_len = static_cast<size_t>(star - sentence - 1);
  if (body_len > kMaxSentence) return 0;
  char body[kMaxSentence + 1];
  memcpy(body, sentence + 1, body_len);
  body[body_len] = '\0';

  char *fields[kMaxFields];
  int n = 0;
  char *p = body;
  fields[n++] = p;
  for (; *p; p++) {
    if (*p == ',') {
      *p = '\0';
      if (n == kMaxFields) break;
      fields[n++] = p + 1;
    }
  }

  // Talker (2 chars) plus sentence type (3 chars); proprietary $P... skipped.
  const char *id = fields[0];
  if (strlen(id) != 5 || id[0] == 'P') return 0;
  sentences_++;
  const char *type = id + 2;
  if (strcmp(type, "RMC") == 0) return handle_rmc(fields, n);
  if (strcmp(type, "GGA") == 0) return handle_gga(fields, n);
  if (strcmp(type, "GSA") == 0) return handle_gsa(fields, n);
  if (strcmp(type, "GSV") == 0) return handle_gsv(id, fields, n);
  if (strcmp(type, "TXT") == 0) return handle_txt(fields, n);
  return 0;
}

// $xxRMC,time,status,lat,N/S,lon,E/W,speed,course,date,magvar,E/W[,mode]
uint8_t NmeaParser::handle_rmc(char **f, int n) {
  uint8_t updated = kUpdatedTime | kUpdatedPosition;

  UtcTime t = {};
  nav_.time_valid = parse_time(field(f, n, 1), &t) && parse_date(field(f, n, 9), &t) &&
                    t.year >= kMinValidYear;
  if (nav_.time_valid) nav_.utc = t;

  // Mode indicator (NMEA 2.3+): 'E' estimated and 'N' not valid.
  const char mode = field(f, n, 12)[0];
  double lat, lon;
  const bool ok = field(f, n, 2)[0] == 'A' && mode != 'E' && mode != 'N' &&
                  parse_coordinate(field(f, n, 3), field(f, n, 4), true, &lat) &&
                  parse_coordinate(field(f, n, 5), field(f, n, 6), false, &lon);
  nav_.position_valid = ok;
  if (ok) {
    nav_.latitude_deg = lat;
    nav_.longitude_deg = lon;
  }

  double v;
  nav_.speed_valid = ok && parse_decimal(field(f, n, 7), &v);
  if (nav_.speed_valid) nav_.speed_knots = static_cast<float>(v);
  nav_.course_valid = ok && parse_decimal(field(f, n, 8), &v);
  if (nav_.course_valid) nav_.course_deg = static_cast<float>(v);
  return updated;
}

// $xxGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,geoid,M,age,station
uint8_t NmeaParser::handle_gga(char **f, int n) {
  unsigned quality = 0;
  if (!parse_uint(field(f, n, 6), &quality) || quality > 9) quality = 0;
  nav_.fix_quality = static_cast<uint8_t>(quality);

  unsigned sats = 0;
  nav_.satellites = parse_uint(field(f, n, 7), &sats) && sats < 256 ? static_cast<uint8_t>(sats) : 0;

  double v;
  nav_.hdop_valid = parse_decimal(field(f, n, 8), &v) && v > 0;
  if (nav_.hdop_valid) nav_.hdop = static_cast<float>(v);

  // Quality 6 is dead reckoning, not a measured position.
  const bool measured = quality != 0 && quality != 6;
  double lat, lon;
  const bool ok = measured && parse_coordinate(field(f, n, 2), field(f, n, 3), true, &lat) &&
                  parse_coordinate(field(f, n, 4), field(f, n, 5), false, &lon);
  nav_.position_valid = ok;
  if (ok) {
    nav_.latitude_deg = lat;
    nav_.longitude_deg = lon;
  }
  nav_.altitude_valid = ok && parse_decimal(field(f, n, 9), &v);
  if (nav_.altitude_valid) nav_.altitude_m = static_cast<float>(v);
  return kUpdatedPosition | kUpdatedQuality;
}

// $xxGSA,mode,fix_type,sv1..sv12,pdop,hdop,vdop[,system]
uint8_t NmeaParser::handle_gsa(char **f, int n) {
  unsigned type = 0;
  if (!parse_uint(field(f, n, 2), &type) || type < 1 || type > 3) return 0;
  nav_.fix_type = static_cast<uint8_t>(type);
  return kUpdatedQuality;
}

// $xxGSV,messages,message,in_view{,prn,elevation,azimuth,cn0}x0-4[,signal]
// A constellation's satellites span `messages` sentences; totals are taken
// from complete cycles only.
uint8_t NmeaParser::handle_gsv(const char *talker, char **f, int n) {
  unsigned messages = 0, message = 0, in_view = 0;
  if (!parse_uint(field(f, n, 1), &messages) || !parse_uint(field(f, n, 2), &message) ||
      !parse_uint(field(f, n, 3), &in_view) || messages < 1 || message < 1 ||
      message > messages || in_view > 99) {
    return 0;
  }

  int slot = kConstellations - 1;
  if (strncmp(talker, "GP", 2) == 0) {
    slot = 0;
  } else if (strncmp(talker, "GL", 2) == 0) {
    slot = 1;
  } else if (strncmp(talker, "GA", 2) == 0) {
    slot = 2;
  } else if (strncmp(talker, "BD", 2) == 0 || strncmp(talker, "GB", 2) == 0) {
    slot = 3;
  } else if (strncmp(talker, "GQ", 2) == 0) {
    slot = 4;
  }

  GsvCycle &pending = gsv_pending_[slot];
  if (message == 1) pending = GsvCycle{};
  pending.in_view = static_cast<uint8_t>(in_view);
  // Satellite blocks of four fields start at index 4; a trailing signal ID
  // field (NMEA 4.10) never forms a complete block.
  for (int i = 4; i + 3 < n; i += 4) {
    unsigned cn0 = 0;
    if (parse_uint(f[i + 3], &cn0) && cn0 > 0 && cn0 < 100) {
      if (pending.with_signal < 255) pending.with_signal++;
      if (cn0 > pending.max_cn0) pending.max_cn0 = static_cast<uint8_t>(cn0);
    }
  }
  if (message != messages) return 0;

  gsv_done_[slot] = pending;
  unsigned view = 0, signal = 0, best = 0;
  for (const GsvCycle &c : gsv_done_) {
    view += c.in_view;
    signal += c.with_signal;
    if (c.max_cn0 > best) best = c.max_cn0;
  }
  nav_.satellites_in_view = static_cast<uint8_t>(view > 255 ? 255 : view);
  nav_.satellites_with_signal = static_cast<uint8_t>(signal > 255 ? 255 : signal);
  nav_.max_cn0 = static_cast<uint8_t>(best);
  return kUpdatedDiagnostics;
}

// $xxTXT,total,number,type,text. Antenna status and receiver identification
// ("SW=", "HW=", "IC=") are used; other text is ignored. The text field may
// itself contain commas, so it is rejoined from the remaining fields.
uint8_t NmeaParser::handle_txt(char **f, int n) {
  char text[sizeof(nav_.receiver_info)] = {};
  for (int i = 4; i < n; i++) {
    if (i > 4) strncat(text, ",", sizeof(text) - strlen(text) - 1);
    strncat(text, f[i], sizeof(text) - strlen(text) - 1);
  }
  if (strncmp(text, "SW=", 3) == 0 || strncmp(text, "HW=", 3) == 0 ||
      strncmp(text, "IC=", 3) == 0) {
    memcpy(nav_.receiver_info, text, sizeof(nav_.receiver_info));
    return kUpdatedDiagnostics;
  }
  if (!strstr(text, "ANTENNA") && !strstr(text, "ANTSTATUS")) return 0;
  Antenna a = Antenna::Unknown;
  if (strstr(text, "OPEN")) {
    a = Antenna::Open;
  } else if (strstr(text, "SHORT")) {
    a = Antenna::Short;
  } else if (strstr(text, "OK")) {
    a = Antenna::Ok;
  }
  if (a == Antenna::Unknown) return 0;
  nav_.antenna = a;
  return kUpdatedDiagnostics;
}

}  // namespace fy::gps
