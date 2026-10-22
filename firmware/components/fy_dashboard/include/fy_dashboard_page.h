// SPDX-License-Identifier: MIT
//
// The status page's HTTP server, shared by the Wi-Fi access point and the USB
// dashboard. It runs while either needs it. C-compatible for fy_usb.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the server on first use; each successful call needs a matching
// fy_dashboard_page_stop().
esp_err_t fy_dashboard_page_start(void);

// Stops the server when no user remains.
void fy_dashboard_page_stop(void);

#ifdef __cplusplus
}
#endif
