// SPDX-License-Identifier: MIT
//
// Single-button navigation, following Meshtastic's scheme on Heltec boards:
//   short press      next screen; in a menu, next item
//   hold ~0.5 s      open the current screen's menu; in a menu, activate item
//   hold ~4 s        power off (only more than 30 s after boot)
// Pure C++ (host-testable): the caller classifies presses and performs the
// returned actions.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::ui {

constexpr uint32_t kLongPressMs = 500;
constexpr uint32_t kPowerOffHoldMs = 4000;
constexpr uint32_t kPowerOffMinUptimeMs = 30000;

enum class Screen : uint8_t { Home = 0, Detections, Data, Device, Count };
enum class Overlay : uint8_t { None = 0, DataMenu, EraseConfirm, DeviceMenu };
enum class Press : uint8_t { Short, Long, PowerOff };
enum class Action : uint8_t {
  None,
  SaveOpen,
  EraseAll,
  PowerOff,
  SelectScanning,
  SelectStandby,
  ToggleDashboard,  // start the Wi-Fi dashboard if it is off, stop it if on
};

struct UiState {
  Screen screen = Screen::Home;
  Overlay overlay = Overlay::None;
  uint8_t item = 0;  // highlighted menu item
  char message[22] = {};  // transient notice, shown until message_until_ms
  uint32_t message_until_ms = 0;
};

// Classifies a completed press by how long the button was held.
Press classify_press(uint32_t held_ms, uint32_t uptime_ms);

// Applies a press and returns the action the caller must perform.
Action handle_press(UiState &state, Press press, uint32_t now_ms);

// Shows a transient notice.
void show_message(UiState &state, const char *text, uint32_t now_ms, uint32_t duration_ms = 2500);
bool message_active(const UiState &state, uint32_t now_ms);

// Menu items for an overlay (empty for Overlay::None).
const char *const *menu_items(Overlay overlay, size_t *count);

}  // namespace fy::ui
