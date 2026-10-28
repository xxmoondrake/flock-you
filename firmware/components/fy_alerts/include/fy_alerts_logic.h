// SPDX-License-Identifier: MIT
//
// Detection alerts: the owner's preferences, and upstream flock-you's buzzer
// sounds and cadence (main.cpp: tierChirp, heartbeatBeep, startupBeep,
// fyAddDetection, heartbeatTick). Pure C++ (host-testable).

#pragma once

#include <cstdint>

namespace fy::alerts {

// Upstream's confidence tiers, 0 (weakest) to 4; each signature rule names
// its tier (fy::RuleInfo::upstream_tier).
constexpr uint8_t kTiers = 5;
constexpr uint8_t kAllTiers = (1u << kTiers) - 1;

// The owner's alert preferences. The master switch silences the buzzer and
// the screen flash and keeps their own settings for when it is back on; the
// tier mask is upstream's beep mask.
struct Settings {
  bool enabled = true;  // master switch
  bool buzzer = true;
  bool flash = true;
  uint8_t tiers = kAllTiers;  // bit N: tier N may sound

  bool buzzer_on() const { return enabled && buzzer; }
  bool flash_on() const { return enabled && flash; }
  bool tier_audible(uint8_t tier) const { return tier < kTiers && ((tiers >> tier) & 1u); }
};

// One byte: the three switches, then the tier mask.
uint8_t pack(const Settings &s);
Settings unpack(uint8_t bits);

// A tone, or a pause at 0 Hz.
struct Note {
  uint16_t hz;
  uint16_t ms;
};

struct Sound {
  const Note *notes;
  uint8_t count;
};

// Each tier's sound: the most confident two tiers chirp two rising notes,
// the others blip once, lower for weaker tiers.
Sound tier_sound(uint8_t tier);
// Two even beeps while a detected device stays in range.
Sound heartbeat_sound();
// The tune at start-up.
Sound startup_sound();

constexpr uint32_t kRediscoverMs = 30000;  // an address silent this long is new again
constexpr uint32_t kActiveMs = 3000;       // a device seen this recently is in range
constexpr uint32_t kHeartbeatMs = 10000;   // between heartbeats

// When to sound: a tier's sound for an address that is new, back after
// kRediscoverMs, or seen at a higher tier; then a heartbeat every
// kHeartbeatMs while anything stays in range, in the voice of the highest
// tier seen since the range was last empty.
class Cadence {
 public:
  // Notes a sighting; true when its tier's sound is due.
  bool on_sighting(const uint8_t mac[6], uint8_t tier, uint32_t now_ms);
  // True when a heartbeat is due; *tier is the tier whose mute applies.
  bool heartbeat_due(uint32_t now_ms, uint8_t *tier);
  void heartbeat_played(uint32_t now_ms);

 private:
  struct Entry {
    uint8_t mac[6];
    uint8_t tier;
    bool used;
    uint32_t seen_ms;
  };
  static constexpr int kEntries = 32;  // addresses pushed out count as new again
  Entry seen_[kEntries] = {};
  bool any_ = false;
  uint32_t last_target_ms_ = 0;
  uint8_t window_tier_ = 0;
  uint32_t last_heartbeat_ms_ = 0;
};

}  // namespace fy::alerts
