// SPDX-License-Identifier: MIT
//
// USB dashboard (boards built with CONFIG_FY_USB_DASHBOARD): a composite USB
// device with a CDC-ACM serial port, which becomes the console, and a CDC-NCM
// network interface serving the dashboard. The XIAO starts it
// at boot; the Heltec starts and stops it from its Device menu.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The device's address on the USB network.
#define FY_USB_ADDRESS "192.168.7.1"

// Starts the USB device. The console moves from USB Serial/JTAG to the
// CDC-ACM port. The network interface is 192.168.7.1/24; with the captive
// portal (FY_DASHBOARD_CAPTIVE_PORTAL) its DHCP server offers the device as
// gateway and DNS server so phones open the page in their sign-in window,
// and otherwise it offers only an address. Detection keeps running. A
// 1200-baud "touch" on the serial port (open at 1200 baud, drop DTR) reboots
// into the serial bootloader for flashing. Returns ESP_ERR_NOT_SUPPORTED when
// built without USB networking.
esp_err_t fy_usb_start(void);

// Stops the USB device and returns the port and console to USB Serial/JTAG.
void fy_usb_stop(void);

#ifdef __cplusplus
}
#endif
