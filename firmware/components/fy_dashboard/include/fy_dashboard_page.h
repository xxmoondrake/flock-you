// SPDX-License-Identifier: MIT
//
// The status page's HTTP server, shared by the Wi-Fi access point and the USB
// dashboard. It runs while either needs it. C-compatible for fy_usb.

#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the server on first use; each successful call needs a matching
// fy_dashboard_page_stop(). In the captive-portal test build it also starts
// a DNS server that answers every name with the device's address.
esp_err_t fy_dashboard_page_start(void);

// Stops the server when no user remains.
void fy_dashboard_page_stop(void);

// In the captive-portal test build (CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL),
// makes a dashboard network's DHCP server offer the device as gateway and DNS
// server. Call before the DHCP server starts. Does nothing otherwise.
esp_err_t fy_dashboard_portal_dhcp(esp_netif_t *netif);

#ifdef __cplusplus
}
#endif
