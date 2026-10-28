// SPDX-License-Identifier: MIT

#include "fy_ui_controller.h"

#include <cstdio>

namespace fy::ui {
namespace {

constexpr const char *kDataMenu[] = {"Save open encounters", "Erase all records", "Back"};
constexpr const char *kEraseConfirm[] = {"Cancel", "Erase everything"};
// The renderer marks the selected mode and names the dashboard item "Start
// dashboard" or "Stop dashboard" by state.
constexpr const char *kDeviceMenu[] = {"Scanning", "Standby", "Dashboard", "Alerts", "Back"};
constexpr const char *kDashboardMenu[] = {"USB dashboard", "WiFi dashboard", "Back"};
// The renderer adds each setting's value.
constexpr const char *kAlertsMenu[] = {"Alerts", "Buzzer", "Screen flash", "Test alert", "Back"};

enum DataItem : uint8_t { kSaveOpen = 0, kErase = 1, kBack = 2 };
enum EraseItem : uint8_t { kCancel = 0, kConfirm = 1 };
enum DeviceItem : uint8_t { kScanning = 0, kStandby = 1, kDashboard = 2, kAlerts = 3, kDeviceBack = 4 };
enum DashboardItem : uint8_t { kUsb = 0, kWifi = 1, kDashboardBack = 2 };
enum AlertsItem : uint8_t { kAlertsSwitch = 0, kBuzzer = 1, kFlash = 2, kTest = 3, kAlertsBack = 4 };

}  // namespace

Press classify_press(uint32_t held_ms, uint32_t uptime_ms) {
  if (held_ms >= kPowerOffHoldMs) {
    return uptime_ms >= kPowerOffMinUptimeMs ? Press::PowerOff : Press::PowerOffEarly;
  }
  return held_ms >= kLongPressMs ? Press::Long : Press::Short;
}

const char *const *menu_items(Overlay overlay, size_t *count) {
  switch (overlay) {
    case Overlay::DataMenu:
      *count = sizeof(kDataMenu) / sizeof(kDataMenu[0]);
      return kDataMenu;
    case Overlay::EraseConfirm:
      *count = sizeof(kEraseConfirm) / sizeof(kEraseConfirm[0]);
      return kEraseConfirm;
    case Overlay::DeviceMenu:
      *count = sizeof(kDeviceMenu) / sizeof(kDeviceMenu[0]);
      return kDeviceMenu;
    case Overlay::DashboardMenu:
      *count = sizeof(kDashboardMenu) / sizeof(kDashboardMenu[0]);
      return kDashboardMenu;
    case Overlay::AlertsMenu:
      *count = sizeof(kAlertsMenu) / sizeof(kAlertsMenu[0]);
      return kAlertsMenu;
    default:
      *count = 0;
      return nullptr;
  }
}

void show_message(UiState &state, const char *text, uint32_t now_ms, uint32_t duration_ms) {
  snprintf(state.message, sizeof(state.message), "%s", text);
  state.message_until_ms = now_ms + duration_ms;
}

bool message_active(const UiState &state, uint32_t now_ms) {
  return state.message[0] && static_cast<int32_t>(state.message_until_ms - now_ms) > 0;
}

Action handle_press(UiState &state, Press press, uint32_t now_ms) {
  if (press == Press::PowerOff) return Action::PowerOff;
  if (press == Press::PowerOffEarly) {  // says why nothing happened, and does nothing else
    show_message(state, "Power off after 30 s", now_ms);
    return Action::None;
  }

  size_t count = 0;
  menu_items(state.overlay, &count);

  switch (state.overlay) {
    case Overlay::None:
      if (press == Press::Short) {
        const uint8_t next = static_cast<uint8_t>(state.screen) + 1;
        state.screen = static_cast<Screen>(next % static_cast<uint8_t>(Screen::Count));
      } else if (state.screen == Screen::Data) {
        state.overlay = Overlay::DataMenu;
        state.item = 0;
      } else if (state.screen == Screen::Device) {
        state.overlay = Overlay::DeviceMenu;
        state.item = 0;
      } else {
        show_message(state, "No menu on this screen", now_ms, 1500);
      }
      return Action::None;

    case Overlay::DataMenu:
      if (press == Press::Short) {
        state.item = static_cast<uint8_t>((state.item + 1) % count);
        return Action::None;
      }
      if (state.item == kSaveOpen) {
        state.overlay = Overlay::None;
        return Action::SaveOpen;
      }
      if (state.item == kErase) {
        state.overlay = Overlay::EraseConfirm;
        state.item = kCancel;  // the safe choice is highlighted first
        return Action::None;
      }
      state.overlay = Overlay::None;
      return Action::None;

    case Overlay::EraseConfirm:
      if (press == Press::Short) {
        state.item = static_cast<uint8_t>((state.item + 1) % count);
        return Action::None;
      }
      state.overlay = Overlay::None;
      if (state.item == kConfirm) return Action::EraseAll;
      show_message(state, "Erase cancelled", now_ms);
      return Action::None;

    case Overlay::DeviceMenu:
      if (press == Press::Short) {
        state.item = static_cast<uint8_t>((state.item + 1) % count);
        return Action::None;
      }
      if (state.item == kAlerts) {
        state.overlay = Overlay::AlertsMenu;
        state.item = 0;
        return Action::None;
      }
      if (state.item == kDashboard && !state.dashboard_running) {
        state.overlay = Overlay::DashboardMenu;  // choose USB or Wi-Fi
        state.item = 0;
        return Action::None;
      }
      state.overlay = Overlay::None;
      switch (state.item) {
        case kScanning: return Action::SelectScanning;
        case kStandby: return Action::SelectStandby;
        case kDashboard: return Action::StopDashboard;
        default: return Action::None;
      }

    case Overlay::DashboardMenu:
      if (press == Press::Short) {
        state.item = static_cast<uint8_t>((state.item + 1) % count);
        return Action::None;
      }
      if (state.item == kDashboardBack) {
        state.overlay = Overlay::DeviceMenu;  // back where the submenu opened
        state.item = kDashboard;
        return Action::None;
      }
      state.overlay = Overlay::None;
      return state.item == kUsb ? Action::StartUsbDashboard : Action::StartWifiDashboard;

    case Overlay::AlertsMenu:
      if (press == Press::Short) {
        state.item = static_cast<uint8_t>((state.item + 1) % count);
        return Action::None;
      }
      // A setting changes in place, so the menu shows its new value; a
      // test plays over the menu and returns to it.
      switch (state.item) {
        case kAlertsSwitch: return Action::ToggleAlerts;
        case kBuzzer: return Action::ToggleBuzzer;
        case kFlash: return Action::ToggleFlash;
        case kTest: return Action::TestAlert;
        default:
          state.overlay = Overlay::DeviceMenu;  // back where the submenu opened
          state.item = kAlerts;
          return Action::None;
      }
  }
  return Action::None;
}

}  // namespace fy::ui
