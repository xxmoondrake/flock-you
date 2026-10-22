// SPDX-License-Identifier: MIT
//
// The access point is WPA3-Personal only (SAE, protected management frames
// required). Its SSID and passkey are generated per device on first use and
// kept in NVS; the passkey is never logged. The status page is plain HTTP
// inside that network until the planned HTTPS dashboard replaces it.

#include "fy_dashboard.h"
#include "fy_dashboard_page.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "fy_battery.h"
#include "fy_dashboard_text.h"
#include "fy_detector.h"
#include "fy_gps.h"
#include "fy_recorder.h"
#include "fy_signatures.h"

#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
#include "lwip/sockets.h"

#include "fy_portal.h"
#endif

namespace {

const char *TAG = "dashboard";

constexpr const char *kNvsNamespace = "fy_dash";
constexpr uint8_t kMaxClients = 2;
constexpr size_t kPageCap = 4096;
constexpr int kTypeCount = static_cast<int>(fy::DeviceType::Count);

bool netif_initialized = false;
esp_netif_t *ap_netif = nullptr;  // exists only while a session runs
httpd_handle_t server = nullptr;
std::atomic<bool> access_point_on{false};  // also read by the HTTP server task
fy_dashboard_request_hook request_hook = nullptr;

// Written by the device task only; read by others under the lock.
portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
fy_dashboard_status published = {};

char ssid[33];
char passkey[64];
bool passkey_temporary = true;  // unchanged since it was generated or built in
bool passkey_shared = false;    // the build-time passkey, not one per device
bool credentials_ready = false;

// Rendered only by the HTTP server task, so one buffer suffices.
char page[kPageCap];

uint8_t hardware_random_byte(void *) { return static_cast<uint8_t>(esp_random()); }

// A build-time passkey shared by every device built with it, for boards that
// cannot show a generated one (no display). WPA3-Personal needs 8-63
// characters.
constexpr char kSharedPasskey[] = CONFIG_FY_DASHBOARD_PASSKEY;
static_assert(sizeof(kSharedPasskey) == 1 ||
                  (sizeof(kSharedPasskey) >= 9 && sizeof(kSharedPasskey) <= 64),
              "FY_DASHBOARD_PASSKEY must be empty or 8-63 characters");

// Loads this device's SSID and passkey, generating and storing them on first
// use. The hardware RNG is a true random source while the radios are on.
esp_err_t load_credentials() {
  if (credentials_ready) return ESP_OK;
  nvs_handle_t nvs;
  ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &nvs), TAG, "nvs_open");
  bool dirty = false;
  esp_err_t err = ESP_OK;

  size_t ssid_len = sizeof(ssid);
  if (nvs_get_str(nvs, "ssid", ssid, &ssid_len) != ESP_OK) {
    if (!fy::dashboard::make_ssid(CONFIG_FY_DASHBOARD_SSID_PREFIX, hardware_random_byte, nullptr,
                                  ssid, sizeof(ssid))) {
      err = ESP_ERR_INVALID_SIZE;
    } else {
      err = nvs_set_str(nvs, "ssid", ssid);
      dirty = true;
    }
  }

  if (err == ESP_OK && kSharedPasskey[0]) {
    snprintf(passkey, sizeof(passkey), "%s", kSharedPasskey);
    passkey_shared = true;
    passkey_temporary = true;  // the default from the build, never changed on the device
  } else if (err == ESP_OK) {
    size_t pass_len = sizeof(passkey);
    uint8_t changed = 0;
    if (nvs_get_str(nvs, "pass", passkey, &pass_len) == ESP_OK) {
      nvs_get_u8(nvs, "changed", &changed);  // absent means unchanged
    } else if (!fy::dashboard::make_passkey(hardware_random_byte, nullptr, passkey,
                                            sizeof(passkey))) {
      err = ESP_ERR_INVALID_SIZE;
    } else {
      err = nvs_set_str(nvs, "pass", passkey);
      if (err == ESP_OK) err = nvs_set_u8(nvs, "changed", 0);
      dirty = true;
    }
    passkey_temporary = changed == 0;
  }

  if (err == ESP_OK && dirty) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) return err;
  if (dirty) {
    ESP_LOGI(TAG, "generated access point %s with a %s passkey", ssid,
             passkey_shared ? "shared build-time" : "temporary");
  }
  credentials_ready = true;
  return ESP_OK;
}

// Device types with at least one device, in signature table order.
int gather_page(fy::dashboard::StatusPage &p, fy::dashboard::PageRow *rows) {
  p = {};
  p.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
  p.idle_minutes = static_cast<uint16_t>(CONFIG_FY_DASHBOARD_IDLE_S / 60);
  // The passkey note concerns the Wi-Fi access point, so it shows only during
  // a session. A Wi-Fi session pauses detection; the USB dashboard does not.
  const bool wifi_session = access_point_on;
  p.passkey_temporary = wifi_session && passkey_temporary;
  p.passkey_shared = wifi_session && passkey_shared;
  p.detection_paused = wifi_session;

  fy_battery_status b;
  fy_battery_get_status(&b);
  p.battery_available = b.available;
  p.battery_valid = b.valid;
  p.battery_mv = b.mv;
  p.battery_percent = b.percent;

  fy_gps_status g;
  fy_gps_get_status(&g);
  p.gps_available = g.available;
  p.gps_fix = g.position_current;
  p.fix_type = g.nav.fix_type;
  p.satellites = g.nav.satellites;

  fy_detector_status d;
  fy_detector_get_status(&d);
  p.reported = d.reported;
  p.reported_specific = d.reported_specific;

  fy_recorder_status r;
  fy_recorder_get_status(&r);
  p.storage_available = r.available;
  p.storage_percent = r.used_percent;
  p.records_used = r.used;
  p.records_capacity = r.capacity;
  p.counts_saturated = r.session_saturated || r.stored_saturated;
  int n = 0;
  for (int t = 0; t < kTypeCount; t++) {
    if (!r.session_devices[t] && !r.stored_devices[t]) continue;
    const fy::DeviceTypeInfo &info = fy::device_type_info(static_cast<fy::DeviceType>(t));
    rows[n++] = {info.name, info.possible, r.session_devices[t], r.stored_devices[t]};
  }
  p.rows = rows;
  p.row_count = n;
  return n;
}

#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
// The device's address on the connection a request arrived on. The server
// listens dual-stack, so IPv4 clients appear as IPv4-mapped IPv6 addresses.
void local_address(httpd_req_t *req, char *out, size_t cap) {
  out[0] = '\0';
  sockaddr_storage addr = {};
  socklen_t len = sizeof(addr);
  if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    return;
  }
  if (addr.ss_family == AF_INET) {
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&addr)->sin_addr, out, cap);
  } else if (addr.ss_family == AF_INET6) {
    const auto *a6 = reinterpret_cast<sockaddr_in6 *>(&addr);
    inet_ntop(AF_INET, &a6->sin6_addr.un.u32_addr[3], out, cap);
  }
}

// Redirects everything except the status page itself, so phones treat the
// network as a captive portal and open the page in their sign-in window.
bool redirect_if_needed(httpd_req_t *req) {
  char local_ip[16];
  local_address(req, local_ip, sizeof(local_ip));
  char host[64] = "";
  const esp_err_t err = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) snprintf(host, sizeof(host), "?");
  if (fy::portal::route_request(host, req->uri, local_ip) == fy::portal::Route::Page) return false;
  char location[32];
  snprintf(location, sizeof(location), "http://%s/", local_ip[0] ? local_ip : "192.168.4.1");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, nullptr, 0);
  return true;
}

// A DNS server that answers every name with this device's address on the
// asking client's network. Both dashboard networks are /24s with the device
// at .1, so the answer is the client's address with the last byte set to 1.
std::atomic<bool> dns_running{false};
TaskHandle_t dns_task_handle = nullptr;
int dns_socket = -1;

void dns_task(void *) {
  uint8_t query[512];
  uint8_t reply[512];
  while (dns_running) {
    sockaddr_in src = {};
    socklen_t src_len = sizeof(src);
    const int n = recvfrom(dns_socket, query, sizeof(query), 0,
                           reinterpret_cast<sockaddr *>(&src), &src_len);
    if (n <= 0) continue;  // timeout: check whether to keep running
    uint8_t ip[4];
    memcpy(ip, &src.sin_addr.s_addr, sizeof(ip));
    ip[3] = 1;
    const size_t len = fy::portal::build_dns_reply(query, static_cast<size_t>(n), ip, reply,
                                                   sizeof(reply));
    if (len) {
      sendto(dns_socket, reply, len, 0, reinterpret_cast<sockaddr *>(&src), src_len);
    }
  }
  close(dns_socket);
  dns_socket = -1;
  dns_task_handle = nullptr;
  vTaskDelete(nullptr);
}

esp_err_t start_dns() {
  dns_running = true;
  if (dns_task_handle) return ESP_OK;  // still running from before
  dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (dns_socket < 0) return ESP_FAIL;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  timeval timeout = {1, 0};
  if (bind(dns_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      setsockopt(dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
      xTaskCreate(dns_task, "fy_dns", 4096, nullptr, 3, &dns_task_handle) != pdPASS) {
    close(dns_socket);
    dns_socket = -1;
    dns_running = false;
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "captive portal: answering every DNS name, redirecting other pages");
  return ESP_OK;
}

void stop_dns() { dns_running = false; }  // the task exits within a second
#endif  // CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL

esp_err_t status_get(httpd_req_t *req) {
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
  if (redirect_if_needed(req)) return ESP_OK;
#endif
  if (request_hook) request_hook();  // only page loads count as activity
  fy::dashboard::PageRow rows[kTypeCount];
  fy::dashboard::StatusPage p;
  gather_page(p, rows);
  const size_t len = fy::dashboard::render_status_page(p, page, sizeof(page));
  if (!len) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
  return httpd_resp_send(req, page, static_cast<ssize_t>(len));
}

esp_err_t start_access_point() {
  if (!netif_initialized) {
    const esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    netif_initialized = true;
  }
  if (!ap_netif) {
    ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_FAIL;
    ESP_RETURN_ON_ERROR(fy_dashboard_portal_dhcp(ap_netif), TAG, "portal dhcp");
  }
  wifi_config_t cfg = {};
  const size_t ssid_len = strlen(ssid);
  memcpy(cfg.ap.ssid, ssid, ssid_len);
  cfg.ap.ssid_len = static_cast<uint8_t>(ssid_len);
  memcpy(cfg.ap.password, passkey, strlen(passkey));
  cfg.ap.channel = CONFIG_FY_DASHBOARD_CHANNEL;
  cfg.ap.max_connection = kMaxClients;
  cfg.ap.authmode = WIFI_AUTH_WPA3_PSK;
  cfg.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  cfg.ap.pmf_cfg.capable = true;
  cfg.ap.pmf_cfg.required = true;

  access_point_on = true;  // from here on, stopping must restore capture mode
  ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "esp_wifi_stop");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set_mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "set_config");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
  return ESP_OK;
}

esp_err_t start_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 6144;
  config.max_open_sockets = 3;
  config.lru_purge_enable = true;
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
  config.uri_match_fn = httpd_uri_match_wildcard;  // every path, for redirects
#endif
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");
  httpd_uri_t root = {};
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
  root.uri = "/*";
#else
  root.uri = "/";
#endif
  root.method = HTTP_GET;
  root.handler = status_get;
  const esp_err_t err = httpd_register_uri_handler(server, &root);
  if (err != ESP_OK) {
    httpd_stop(server);
    server = nullptr;
  }
  return err;
}

// The server runs while the Wi-Fi access point or the USB dashboard needs it.
SemaphoreHandle_t page_lock() {
  static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  return lock;
}
int page_users = 0;           // guarded by page_lock()
bool session_holds_page = false;  // the Wi-Fi session's share of the server

void publish(bool active) {
  fy_dashboard_status s = {};
  s.active = active;
  snprintf(s.ssid, sizeof(s.ssid), "%s", ssid);
  s.passkey_temporary = passkey_temporary;
  if (passkey_temporary) snprintf(s.passkey, sizeof(s.passkey), "%s", passkey);
  esp_netif_ip_info_t ip = {};
  if (active && ap_netif && esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
    snprintf(s.address, sizeof(s.address), IPSTR, IP2STR(&ip.ip));
  }
  portENTER_CRITICAL(&status_lock);
  published = s;
  portEXIT_CRITICAL(&status_lock);
}

}  // namespace

esp_err_t fy_dashboard_page_start(void) {
  xSemaphoreTake(page_lock(), portMAX_DELAY);
  esp_err_t err = ESP_OK;
  if (page_users == 0) {
    err = start_server();
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
    if (err == ESP_OK) err = start_dns();
    if (err != ESP_OK && server) {
      httpd_stop(server);
      server = nullptr;
    }
#endif
  }
  if (err == ESP_OK) page_users++;
  xSemaphoreGive(page_lock());
  return err;
}

void fy_dashboard_page_stop(void) {
  xSemaphoreTake(page_lock(), portMAX_DELAY);
  if (page_users > 0 && --page_users == 0 && server) {
    httpd_stop(server);
    server = nullptr;
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
    stop_dns();
#endif
  }
  xSemaphoreGive(page_lock());
}

esp_err_t fy_dashboard_portal_dhcp(esp_netif_t *netif) {
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
  // Offer the device as gateway and DNS server so phones probe it for
  // internet access and find the portal.
  esp_netif_ip_info_t ip = {};
  ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(netif, &ip), TAG, "ip info");
  uint8_t on = 1;
  ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                             ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &on,
                                             sizeof(on)),
                      TAG, "router offer");
  ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                             ESP_NETIF_DOMAIN_NAME_SERVER, &on, sizeof(on)),
                      TAG, "dns offer");
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4 = ip.ip;
  return esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
#else
  (void)netif;
  return ESP_OK;
#endif
}

esp_err_t fy_dashboard_start() {
#if !CONFIG_FY_WIFI_DASHBOARD
  return ESP_ERR_NOT_SUPPORTED;
#endif
  if (access_point_on) return ESP_OK;
  ESP_RETURN_ON_ERROR(load_credentials(), TAG, "credentials");
  esp_err_t err = start_access_point();
  if (err == ESP_OK) err = fy_dashboard_page_start();
  if (err == ESP_OK) session_holds_page = true;
  if (err != ESP_OK) {
    fy_dashboard_stop();
    return err;
  }
  publish(true);
  fy_dashboard_status s;
  fy_dashboard_get_status(&s);
  ESP_LOGI(TAG, "access point %s on channel %d, status page http://%s/", ssid,
           CONFIG_FY_DASHBOARD_CHANNEL, s.address);
  return ESP_OK;
}

void fy_dashboard_stop() {
  if (session_holds_page) {
    fy_dashboard_page_stop();  // keeps running while the USB dashboard uses it
    session_holds_page = false;
  }
  if (access_point_on) esp_wifi_stop();
  // Release the access point's network interface, DHCP server, and event
  // handlers; the next session creates them again. lwIP's core thread stays,
  // since ESP-IDF cannot deinitialize lwIP once started.
  if (ap_netif) {
    esp_netif_destroy_default_wifi(ap_netif);
    ap_netif = nullptr;
  }
  if (access_point_on) {
    // Back to the mode Wi-Fi capture runs in; the caller resumes capture.
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    access_point_on = false;
    ESP_LOGI(TAG, "access point stopped; free heap %lu bytes",
             static_cast<unsigned long>(esp_get_free_heap_size()));
  }
  publish(false);
}

void fy_dashboard_get_status(fy_dashboard_status *out) {
  portENTER_CRITICAL(&status_lock);
  *out = published;
  portEXIT_CRITICAL(&status_lock);
  out->clients = 0;
  if (out->active) {
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) out->clients = static_cast<uint8_t>(list.num);
  }
}

void fy_dashboard_set_request_hook(fy_dashboard_request_hook hook) { request_hook = hook; }
