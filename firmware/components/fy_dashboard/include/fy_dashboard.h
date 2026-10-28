// SPDX-License-Identifier: MIT
//
// Wi-Fi dashboard: a WPA3 access point serving the dashboard. Started and
// stopped by the device task (fy_device); detection is paused while it runs.

#pragma once

#include <cstdint>

#include "esp_err.h"

// Switches the Wi-Fi radio from capture to the access point and starts the
// dashboard. On first use, generates and stores this device's SSID and
// temporary passkey. Wi-Fi capture must already be stopped.
esp_err_t fy_dashboard_start();

// Stops the dashboard and the access point, releases the access point's
// network interface and DHCP server, and returns the Wi-Fi radio to
// capture-ready mode. Wi-Fi capture is resumed by the caller.
void fy_dashboard_stop();

struct fy_dashboard_status {
  bool active;
  char ssid[33];
  char passkey[64];        // empty once the passkey has been changed
  bool passkey_temporary;  // still the generated passkey
  char address[16];        // the dashboard's IPv4 address
  uint8_t clients;         // connected stations
};

// Copies the current status. Safe from any task.
void fy_dashboard_get_status(fy_dashboard_status *out);

// Called for every logged-in dashboard request and every login attempt, from
// the HTTP server task. It must return quickly.
using fy_dashboard_request_hook = void (*)();
void fy_dashboard_set_request_hook(fy_dashboard_request_hook hook);

// The detection mode for the dashboard's settings, provided by the device
// layer, which owns it: `get` returns true in Standby.
using fy_dashboard_mode_get = bool (*)();
using fy_dashboard_mode_set = void (*)(bool standby);
void fy_dashboard_set_mode_hooks(fy_dashboard_mode_get get, fy_dashboard_mode_set set);
