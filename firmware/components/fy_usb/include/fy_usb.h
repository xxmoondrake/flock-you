// SPDX-License-Identifier: MIT
//
// USB dashboard (boards built with CONFIG_FY_USB_DASHBOARD, the XIAO): a
// composite USB device with a CDC-ACM serial port, which becomes the console,
// and a CDC-NCM network interface serving the dashboard's status page.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the USB device. The console moves from USB Serial/JTAG to the
// CDC-ACM port. The network interface is 192.168.7.1/24 with a DHCP server
// that offers the host an address but no gateway or DNS, so the host keeps
// its own internet connection. Detection keeps running. A 1200-baud "touch"
// on the serial port (open at 1200 baud, drop DTR) reboots into the serial
// bootloader for flashing. Returns ESP_ERR_NOT_SUPPORTED when built without
// USB networking.
esp_err_t fy_usb_start(void);

#ifdef __cplusplus
}
#endif
