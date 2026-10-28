// SPDX-License-Identifier: MIT

#include "fy_alerts_logic.h"

#include <cstddef>
#include <cstring>

namespace fy::alerts {
namespace {

constexpr uint8_t kEnabledBit = 1;
constexpr uint8_t kBuzzerBit = 2;
constexpr uint8_t kFlashBit = 4;
constexpr int kTierShift = 3;

// Upstream's notes (main.cpp): T4_LO/HI_HZ, T3_LO/HI_HZ, T2_HZ, T1_HZ, T0_HZ
// with TIER_NOTE_MS, TIER_GAP_MS, BLIP_MS; HB_BEEP_*; the start-up tune.
constexpr Note kTier4[] = {{2000, 55}, {0, 25}, {2800, 55}};
constexpr Note kTier3[] = {{1400, 55}, {0, 25}, {1800, 55}};
constexpr Note kTier2[] = {{1200, 45}};
constexpr Note kTier1[] = {{800, 45}};
constexpr Note kTier0[] = {{600, 45}};
constexpr Note kHeartbeat[] = {{1500, 70}, {0, 70}, {1500, 70}};
constexpr Note kStartup[] = {{262, 95}, {0, 22}, {523, 95}, {0, 22}, {220, 95}, {0, 22},
                             {440, 95}, {0, 22}, {233, 95}, {0, 22}, {466, 160}};

template <size_t N>
constexpr Sound sound(const Note (&notes)[N]) {
  return {notes, static_cast<uint8_t>(N)};
}

}  // namespace

uint8_t pack(const Settings &s) {
  return static_cast<uint8_t>((s.enabled ? kEnabledBit : 0) | (s.buzzer ? kBuzzerBit : 0) |
                              (s.flash ? kFlashBit : 0) | ((s.tiers & kAllTiers) << kTierShift));
}

Settings unpack(uint8_t bits) {
  Settings s;
  s.enabled = bits & kEnabledBit;
  s.buzzer = bits & kBuzzerBit;
  s.flash = bits & kFlashBit;
  s.tiers = static_cast<uint8_t>((bits >> kTierShift) & kAllTiers);
  return s;
}

Sound tier_sound(uint8_t tier) {
  switch (tier) {
    case 4: return sound(kTier4);
    case 3: return sound(kTier3);
    case 2: return sound(kTier2);
    case 1: return sound(kTier1);
    default: return sound(kTier0);
  }
}

Sound heartbeat_sound() { return sound(kHeartbeat); }

Sound startup_sound() { return sound(kStartup); }

bool Cadence::on_sighting(const uint8_t mac[6], uint8_t tier, uint32_t now_ms) {
  // The range was empty: this sighting starts a new heartbeat voice.
  if (!any_ || now_ms - last_target_ms_ > kActiveMs) window_tier_ = tier;
  if (tier > window_tier_) window_tier_ = tier;
  any_ = true;
  last_target_ms_ = now_ms;

  bool due = true;
  Entry *slot = nullptr;
  for (Entry &e : seen_) {
    if (e.used && memcmp(e.mac, mac, 6) == 0) {
      due = now_ms - e.seen_ms > kRediscoverMs || tier > e.tier;
      if (tier > e.tier) e.tier = tier;
      e.seen_ms = now_ms;
      slot = &e;
      break;
    }
  }
  if (!slot) {
    // A free entry, or else the address seen longest ago.
    slot = &seen_[0];
    for (Entry &e : seen_) {
      if (!e.used) {
        slot = &e;
        break;
      }
      if (now_ms - e.seen_ms > now_ms - slot->seen_ms) slot = &e;
    }
    memcpy(slot->mac, mac, 6);
    slot->tier = tier;
    slot->used = true;
    slot->seen_ms = now_ms;
  }
  // The first heartbeat comes a full interval after the sound.
  if (due) last_heartbeat_ms_ = now_ms;
  return due;
}

bool Cadence::heartbeat_due(uint32_t now_ms, uint8_t *tier) {
  if (!any_ || now_ms - last_target_ms_ > kActiveMs) return false;
  if (now_ms - last_heartbeat_ms_ < kHeartbeatMs) return false;
  *tier = window_tier_;
  return true;
}

void Cadence::heartbeat_played(uint32_t now_ms) { last_heartbeat_ms_ = now_ms; }

}  // namespace fy::alerts
