// SPDX-License-Identifier: MIT
//
// Written in C because the TinyUSB and esp_netif configuration helpers are C
// compound-literal macros.

#include "fy_usb.h"

#include "sdkconfig.h"

#if CONFIG_FY_USB_DASHBOARD

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "vfs_tinyusb.h"

#include "fy_dashboard_page.h"

static const char *TAG = "usb";

// 192.168.7.0/24 is unlikely to collide with a phone's or PC's own networks.
#define FY_USB_IP ESP_IP4TOADDR(192, 168, 7, 1)
#define FY_USB_NETMASK ESP_IP4TOADDR(255, 255, 255, 0)
#define FY_TOUCH_BAUD 1200
#define FY_TX_TIMEOUT_MS 100

// Start stages, recorded as each completes so that a failed start, or a stop,
// undoes exactly what was done, in reverse order.
enum {
    STAGE_DRIVER = 1u << 0,
    STAGE_CDC = 1u << 1,
    STAGE_CONSOLE = 1u << 2,
    STAGE_NET = 1u << 3,
    STAGE_NETIF = 1u << 4,     // the interface exists
    STAGE_NETIF_UP = 1u << 5,  // ...and was started
    STAGE_PAGE = 1u << 6,      // holds a share of the dashboard server
};
static unsigned s_stages;
static bool s_running;  // every stage completed

static esp_netif_t *s_netif;
static volatile bool s_touch_baud;  // the host set the line to 1200 baud

// String descriptors in esp_tinyusb's own order for a CDC + NCM device, so
// the default configuration descriptor's indices still match; tinyusb_net
// fills in the MAC address. Only the network interface's name differs from
// the default ("USB net"): Windows names the adapter "<manufacturer>
// <interface>", so with no interface name it is just the manufacturer's.
#if CONFIG_TINYUSB_MSC_ENABLED || CONFIG_TINYUSB_MTP_ENABLED || !CONFIG_TINYUSB_CDC_ENABLED || \
    !CONFIG_TINYUSB_NET_MODE_NCM
#error "s_usb_strings follows the string order of a CDC + NCM device only"
#endif
_Static_assert(sizeof(CONFIG_TINYUSB_DESC_SERIAL_STRING) > 1,
               "set a serial string: the chip-ID serial comes only with the default strings");
static const char *s_usb_strings[] = {
    (const char[]){0x09, 0x04},               // 0: English (0x0409)
    CONFIG_TINYUSB_DESC_MANUFACTURER_STRING,  // 1
    CONFIG_TINYUSB_DESC_PRODUCT_STRING,       // 2
    CONFIG_TINYUSB_DESC_SERIAL_STRING,        // 3
    CONFIG_TINYUSB_DESC_CDC_STRING,           // 4: console
    "",                                       // 5: network interface
    "",                                       // 6: MAC address
};

// ---- Network interface glue --------------------------------------------------

// lwIP to USB: the frame only has to stay valid for the call.
static esp_err_t netif_transmit(void *handle, void *buffer, size_t len)
{
    (void)handle;
    if (len > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    // Fails while the host has not configured the device; lwIP retries.
    tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(FY_TX_TIMEOUT_MS));
    return ESP_OK;
}

static void netif_free_rx(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

// USB to lwIP. TinyUSB reuses its buffer after this returns, so the frame is
// copied; lwIP frees the copy through netif_free_rx.
static esp_err_t usb_receive(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_netif || len == 0) return ESP_OK;
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_netif, copy, len, copy);
}

static esp_err_t start_netif(const uint8_t device_mac[6])
{
    const esp_err_t init = esp_netif_init();
    if (init != ESP_OK && init != ESP_ERR_INVALID_STATE) return init;

    esp_netif_ip_info_t ip = {
        .ip = {.addr = FY_USB_IP},
        .netmask = {.addr = FY_USB_NETMASK},
        .gw = {.addr = FY_USB_IP},
    };
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP;
    base.ip_info = &ip;
    base.get_ip_event = 0;
    base.lost_ip_event = 0;
    base.if_key = "USB_NCM";
    base.if_desc = "usb";
    base.route_prio = 5;
    esp_netif_driver_ifconfig_t driver = {
        .handle = (void *)1,  // unused, but must not be NULL
        .transmit = netif_transmit,
        .driver_free_rx_buffer = netif_free_rx,
    };
    esp_netif_config_t cfg = {
        .base = &base,
        .driver = &driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) return ESP_ERR_NO_MEM;
    s_stages |= STAGE_NETIF;
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_netif, (uint8_t *)device_mac), TAG, "set_mac");

    // Offer an address only: no gateway and no DNS, so the host does not
    // route its internet traffic to the detector.
    uint8_t off = 0;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                               ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &off,
                                               sizeof(off)),
                        TAG, "no router");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                               ESP_NETIF_DOMAIN_NAME_SERVER, &off, sizeof(off)),
                        TAG, "no dns");
    // The captive portal (the default) offers both after all.
    ESP_RETURN_ON_ERROR(fy_dashboard_portal_dhcp(s_netif), TAG, "portal dhcp");
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    s_stages |= STAGE_NETIF_UP;
    esp_netif_action_connected(s_netif, NULL, 0, NULL);
    return ESP_OK;
}

// ---- Serial bootloader on request ------------------------------------------------

// Hands the internal USB PHY back to USB Serial/JTAG, which the ROM serial
// bootloader uses, and restarts into it.
static void reboot_to_bootloader(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(50));  // let the host finish its request
    ESP_LOGW(TAG, "1200-baud touch: rebooting into the serial bootloader");
    tinyusb_driver_uninstall();
    usb_serial_jtag_ll_phy_enable_external(false);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void on_line_coding(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_touch_baud = event->line_coding_changed_data.p_line_coding->bit_rate == FY_TOUCH_BAUD;
}

// The touch: the host opens the port at 1200 baud and drops DTR. A terminal
// at any other rate never triggers it.
static void on_line_state(int itf, cdcacm_event_t *event)
{
    (void)itf;
    if (s_touch_baud && !event->line_state_changed_data.dtr) {
        xTaskCreate(reboot_to_bootloader, "fy_usb_boot", 3072, NULL, 5, NULL);
    }
}

// ---- Teardown --------------------------------------------------------------------------

// tinyusb_console_deinit() reopens the standard streams on /dev/uart/<n>,
// which does not exist when USB Serial/JTAG is the primary console: the
// streams stay on the CDC port, and the next log after the driver goes away
// crashes. Reopen them on /dev/console, where startup opened them.
static void restore_console(void)
{
    freopen("/dev/console", "r", stdin);
    freopen("/dev/console", "w", stdout);
    freopen("/dev/console", "w", stderr);
    esp_vfs_tusb_cdc_unregister(NULL);
}

// Undoes every completed start stage, newest first, from any partial state.
static void teardown(void)
{
    if (s_stages & STAGE_PAGE) fy_dashboard_page_stop();
    if (s_stages & STAGE_NETIF) {
        // Received frames are dropped from here on, and removing the
        // interface stops lwIP transmitting before the USB stack goes away.
        esp_netif_t *netif = s_netif;
        s_netif = NULL;
        if (s_stages & STAGE_NETIF_UP) esp_netif_action_stop(netif, NULL, 0, NULL);
        esp_netif_destroy(netif);
    }
    if (s_stages & STAGE_CONSOLE) restore_console();  // stdout back to USB Serial/JTAG
    if (s_stages & STAGE_CDC) tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    if (s_stages & STAGE_NET) tinyusb_net_deinit();
    if (s_stages & STAGE_DRIVER) {
        tinyusb_driver_uninstall();
        usb_serial_jtag_ll_phy_enable_external(false);
    }
    s_stages = 0;
    s_running = false;
    s_touch_baud = false;
}

// ---- Start -------------------------------------------------------------------------

static esp_err_t start_stages(void)
{
    uint8_t device_mac[6];
    uint8_t host_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(device_mac, ESP_MAC_ETH), TAG, "read_mac");
    // The host's side of the link needs its own address: a locally
    // administered one derived from the device's.
    ESP_RETURN_ON_ERROR(esp_derive_local_mac(host_mac, device_mac), TAG, "derive_mac");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.string = s_usb_strings;
    tusb_cfg.descriptor.string_count = sizeof(s_usb_strings) / sizeof(s_usb_strings[0]);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "driver_install");
    s_stages |= STAGE_DRIVER;

    const tinyusb_config_cdcacm_t acm = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_line_state_changed = on_line_state,
        .callback_line_coding_changed = on_line_coding,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm), TAG, "cdcacm_init");
    s_stages |= STAGE_CDC;
    ESP_RETURN_ON_ERROR(tinyusb_console_init(TINYUSB_CDC_ACM_0), TAG, "console_init");
    s_stages |= STAGE_CONSOLE;

    tinyusb_net_config_t net = {
        .on_recv_callback = usb_receive,
    };
    memcpy(net.mac_addr, host_mac, sizeof(net.mac_addr));
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net), TAG, "net_init");
    s_stages |= STAGE_NET;
    ESP_RETURN_ON_ERROR(start_netif(device_mac), TAG, "netif");
    ESP_RETURN_ON_ERROR(fy_dashboard_page_start(), TAG, "dashboard");
    s_stages |= STAGE_PAGE;
    return ESP_OK;
}

esp_err_t fy_usb_start(void)
{
    if (s_running) return ESP_OK;
    const esp_err_t err = start_stages();
    if (err != ESP_OK) {
        teardown();  // leaves nothing behind, so a later start can retry
        ESP_LOGE(TAG, "USB dashboard start failed (%s); undone", esp_err_to_name(err));
        return err;
    }
    s_running = true;
    ESP_LOGI(TAG, "USB console and network up; dashboard http://" FY_USB_ADDRESS "/");
    return ESP_OK;
}

// ---- Stop -----------------------------------------------------------------------------

void fy_usb_stop(void)
{
    if (!s_running) return;
    ESP_LOGI(TAG, "stopping USB dashboard");
    teardown();
    ESP_LOGI(TAG, "USB dashboard stopped; console back on USB Serial/JTAG");
}

#else  // CONFIG_FY_USB_DASHBOARD

esp_err_t fy_usb_start(void) { return ESP_ERR_NOT_SUPPORTED; }

void fy_usb_stop(void) {}

#endif  // CONFIG_FY_USB_DASHBOARD
