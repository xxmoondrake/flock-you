// SPDX-License-Identifier: MIT

#include "fy_ui_alerts.h"

#include <cstring>

namespace fy::ui {

FlashFrame flash_frame(uint32_t elapsed_ms) {
  const uint32_t step = elapsed_ms / kFlashStepMs;
  if (step >= kFlashSteps) return FlashFrame::Done;
  return step % 2 ? FlashFrame::Inverted : FlashFrame::Regular;
}

bool AlertGate::admit(const uint8_t mac[6], uint32_t now_ms) {
  Entry *slot = nullptr;
  for (Entry &e : recent_) {
    if (e.used && memcmp(e.mac, mac, 6) == 0) {
      if (now_ms - e.at_ms < kAlertRepeatMs) return false;
      slot = &e;
      break;
    }
  }
  if (!slot) {
    // A free entry, or else the one that alerted longest ago.
    slot = &recent_[0];
    for (Entry &e : recent_) {
      if (!e.used) {
        slot = &e;
        break;
      }
      if (now_ms - e.at_ms > now_ms - slot->at_ms) slot = &e;
    }
  }
  memcpy(slot->mac, mac, 6);
  slot->at_ms = now_ms;
  slot->used = true;
  return true;
}

}  // namespace fy::ui
