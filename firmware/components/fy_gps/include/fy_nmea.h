// SPDX-License-Identifier: MIT
//
// NMEA 0183 parser for GNSS receivers (RMC, GGA, GSA from any talker). Pure
// C++ with no ESP-IDF dependencies so it can be tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::gps {

struct UtcTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t millisecond;
};

// Antenna state as reported by the receiver's text sentences
// ("$GPTXT,...,ANTENNA OPEN" or "ANTSTATUS=OPEN"). A passive antenna draws no
// bias current, so some receivers report Open even when it works; read it
// together with the satellite counts.
enum class Antenna : uint8_t { Unknown = 0, Ok, Open, Short };

// Latest navigation data. Time and position validity are separate states: a
// receiver can know the time before it has a position, and either can be
// lost independently.
struct NavData {
  bool time_valid = false;
  UtcTime utc = {};

  bool position_valid = false;
  double latitude_deg = 0;   // positive north
  double longitude_deg = 0;  // positive east

  uint8_t fix_quality = 0;  // GGA: 0 none, 1 GNSS, 2 DGNSS, 4 RTK, 5 float RTK, 6 estimated
  uint8_t fix_type = 0;     // GSA: 1 none, 2 2D, 3 3D; 0 when not reported
  uint8_t satellites = 0;   // GGA satellites used

  bool hdop_valid = false;
  float hdop = 0;
  bool altitude_valid = false;
  float altitude_m = 0;  // above mean sea level
  bool speed_valid = false;
  float speed_knots = 0;
  bool course_valid = false;
  float course_deg = 0;

  // Receiver diagnostics, from complete GSV cycles summed over constellations.
  uint8_t satellites_in_view = 0;
  uint8_t satellites_with_signal = 0;  // in view with a nonzero C/N0
  uint8_t max_cn0 = 0;                 // strongest C/N0, dB-Hz
  Antenna antenna = Antenna::Unknown;
  // Last identification text from the receiver (for example "SW=URANUS5,..."
  // in reply to a version query); empty until one arrives.
  char receiver_info[48] = {};
};

// Bits returned by NmeaParser::feed() and parse_sentence().
constexpr uint8_t kUpdatedTime = 0x01;      // an RMC sentence updated time validity
constexpr uint8_t kUpdatedPosition = 0x02;  // an RMC or GGA sentence updated position validity
constexpr uint8_t kUpdatedQuality = 0x04;   // fix quality, type, satellites, or HDOP
constexpr uint8_t kUpdatedDiagnostics = 0x08;  // satellites in view or antenna state

// Dates before this year are treated as invalid: receivers report placeholder
// dates (for example 1980) before they have satellite time.
constexpr uint16_t kMinValidYear = 2025;

class NmeaParser {
 public:
  // Feeds one received byte. When it completes a valid sentence, returns the
  // kUpdated* bits for what changed; otherwise returns 0.
  uint8_t feed(char c);

  // Parses one complete sentence starting at '$', without line ending.
  uint8_t parse_sentence(const char *sentence);

  const NavData &data() const { return nav_; }
  uint32_t sentences() const { return sentences_; }
  uint32_t checksum_errors() const { return checksum_errors_; }
  uint32_t overflows() const { return overflows_; }

 private:
  static constexpr size_t kMaxSentence = 100;  // NMEA allows 82; leave margin

  uint8_t handle_rmc(char **f, int n);
  uint8_t handle_gga(char **f, int n);
  uint8_t handle_gsa(char **f, int n);
  uint8_t handle_gsv(const char *talker, char **f, int n);
  uint8_t handle_txt(char **f, int n);

  // Per-constellation GSV state: the cycle in progress and the last complete
  // cycle. Slots: GP, GL, GA, BD/GB, GQ, other.
  static constexpr int kConstellations = 6;
  struct GsvCycle {
    uint8_t in_view = 0;
    uint8_t with_signal = 0;
    uint8_t max_cn0 = 0;
  };
  GsvCycle gsv_pending_[kConstellations] = {};
  GsvCycle gsv_done_[kConstellations] = {};

  NavData nav_;
  char line_[kMaxSentence + 1] = {};
  size_t len_ = 0;
  bool in_sentence_ = false;
  uint32_t sentences_ = 0;
  uint32_t checksum_errors_ = 0;
  uint32_t overflows_ = 0;
};

}  // namespace fy::gps
