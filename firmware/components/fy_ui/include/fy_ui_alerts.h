// SPDX-License-Identifier: MIT
//
// The screen flash for detection alerts: which specific matches flash the
// screen, and the image sequence. The preferences and the buzzer are
// fy_alerts'. Pure C++ (host-testable).

#pragma once

#include <cstdint>

namespace fy::ui {

// Provisional timings, to be tuned on the device.
constexpr uint32_t kFlashStepMs = 300;      // each frame of the image sequence
constexpr uint32_t kFlashSteps = 4;         // regular and inverted in turn
constexpr uint32_t kAlertRepeatMs = 60000;  // the same address alerts at most this often

// The start-up image, shown regular, inverted, regular, inverted.
enum class FlashFrame : uint8_t { Regular, Inverted, Done };
FlashFrame flash_frame(uint32_t elapsed_ms);

// Lets each address flash the screen at most once per kAlertRepeatMs, so a
// device in range for minutes does not flash it continuously. Remembers the most recent
// few addresses; an address pushed out alerts again.
class AlertGate {
 public:
  bool admit(const uint8_t mac[6], uint32_t now_ms);

 private:
  struct Entry {
    uint8_t mac[6];
    uint32_t at_ms;
    bool used;
  };
  static constexpr int kEntries = 8;
  Entry recent_[kEntries] = {};
};

}  // namespace fy::ui
