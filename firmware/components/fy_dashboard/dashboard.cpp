// SPDX-License-Identifier: MIT
//
// The access point is WPA3-Personal only (SAE, protected management frames
// required). Its SSID and passkey are generated per device on first use and
// kept in NVS; the passkey is never logged.
//
// The dashboard needs a login on both transports. Every device starts with
// the build-time default password and must replace it on first login; the
// replacement is stored as a salted PBKDF2-HMAC-SHA256 hash; sessions live
// in RAM only. Pages are plain HTTP: phones' sign-in windows, which make the
// dashboard open by itself, refuse a certificate the device signs itself,
// and no public authority can sign one for it.

#include "fy_dashboard.h"
#include "fy_dashboard_page.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

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
#include "psa/crypto.h"
#include "sdkconfig.h"

#include "fy_battery.h"
#include "fy_board.h"
#include "fy_dashboard_auth.h"
#include "fy_dashboard_export.h"
#include "fy_dashboard_notes.h"
#include "fy_dashboard_text.h"
#include "fy_detector.h"
#include "fy_gps.h"
#include "fy_recorder.h"
#include "fy_store.h"

#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
#include "lwip/sockets.h"

#include "fy_portal.h"
#endif

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");

namespace {

using fy::dashboard::LoginNotice;
using fy::dashboard::PasswordProblem;

const char *TAG = "dashboard";

constexpr const char *kNvsNamespace = "fy_dash";
constexpr uint8_t kMaxClients = 2;
constexpr size_t kPageCap = 4096;
constexpr size_t kBodyCap = 512;
constexpr const char *kCookie = "fy_session";

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

// Used only by the HTTP server task, which handles one request at a time.
char page[kPageCap];
fy::dashboard::Sessions sessions;
fy::dashboard::LoginThrottle throttle;

uint8_t hardware_random_byte(void *) { return static_cast<uint8_t>(esp_random()); }
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

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

// ---- Dashboard password ---------------------------------------------------------------

constexpr char kDefaultPassword[] = CONFIG_FY_DASHBOARD_PASSWORD;
static_assert(sizeof(kDefaultPassword) - 1 >= fy::dashboard::kMinPasswordLen &&
                  sizeof(kDefaultPassword) - 1 <= fy::dashboard::kMaxPasswordLen,
              "FY_DASHBOARD_PASSWORD must be 8-64 characters");

// Takes about a quarter of a second per check on the ESP32-S3.
constexpr uint32_t kPbkdf2Iterations = 10000;

struct StoredPassword {
  uint8_t salt[16];
  uint8_t hash[32];
  uint32_t iterations;
};

StoredPassword stored_password;
bool password_custom = false;  // a replacement is stored; else the default applies
bool password_loaded = false;

bool derive(const char *password, const uint8_t *salt, size_t salt_len, uint32_t iterations,
            uint8_t out[32]) {
  if (psa_crypto_init() != PSA_SUCCESS) return false;
  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
  if (st == PSA_SUCCESS) {
    st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations);
  }
  if (st == PSA_SUCCESS) {
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
  }
  if (st == PSA_SUCCESS) {
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                        reinterpret_cast<const uint8_t *>(password),
                                        strlen(password));
  }
  if (st == PSA_SUCCESS) st = psa_key_derivation_output_bytes(&op, out, 32);
  psa_key_derivation_abort(&op);
  if (st != PSA_SUCCESS) ESP_LOGE(TAG, "password hashing failed (%d)", static_cast<int>(st));
  return st == PSA_SUCCESS;
}

void load_password() {
  if (password_loaded) return;
  password_loaded = true;
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;  // nothing stored yet
  size_t len = sizeof(stored_password);
  password_custom = nvs_get_blob(nvs, "pw", &stored_password, &len) == ESP_OK &&
                    len == sizeof(stored_password) && stored_password.iterations > 0;
  nvs_close(nvs);
}

bool password_is_default() {
  load_password();
  return !password_custom;
}

bool verify_password(const char *candidate) {
  load_password();
  if (!password_custom) {
    const size_t len = strlen(candidate);
    return len == sizeof(kDefaultPassword) - 1 &&
           fy::dashboard::constant_time_equal(candidate, kDefaultPassword, len);
  }
  uint8_t hash[32];
  return derive(candidate, stored_password.salt, sizeof(stored_password.salt),
                stored_password.iterations, hash) &&
         fy::dashboard::constant_time_equal(hash, stored_password.hash, sizeof(hash));
}

esp_err_t store_password(const char *password) {
  StoredPassword p = {};
  esp_fill_random(p.salt, sizeof(p.salt));
  p.iterations = kPbkdf2Iterations;
  if (!derive(password, p.salt, sizeof(p.salt), p.iterations, p.hash)) return ESP_FAIL;
  nvs_handle_t nvs;
  ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &nvs), TAG, "nvs_open");
  esp_err_t err = nvs_set_blob(nvs, "pw", &p, sizeof(p));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) return err;
  stored_password = p;
  password_custom = true;
  ESP_LOGI(TAG, "dashboard password changed");
  return ESP_OK;
}

// ---- Status ---------------------------------------------------------------------------

void gather_status(fy::dashboard::Status &s) {
  s = {};
  s.board = fy_board_name();
  s.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
  s.wifi_session = access_point_on;
  s.detection_paused = access_point_on;  // a Wi-Fi session pauses detection
  s.idle_minutes = static_cast<uint16_t>(CONFIG_FY_DASHBOARD_IDLE_S / 60);
  s.passkey_temporary = access_point_on && passkey_temporary;

  fy_battery_status b;
  fy_battery_get_status(&b);
  s.battery_available = b.available;
  s.battery_valid = b.valid;
  s.battery_mv = b.mv;
  s.battery_percent = b.percent;

  fy_gps_status g;
  fy_gps_get_status(&g);
  s.gps_available = g.available;
  s.gps_fix = g.position_current;
  s.fix_type = g.nav.fix_type;
  s.satellites = g.nav.satellites;

  fy_detector_status d;
  fy_detector_get_status(&d);
  s.reported = d.reported;
  s.reported_specific = d.reported_specific;

  fy_recorder_status r;
  fy_recorder_get_status(&r);
  s.storage_available = r.available;
  s.storage_percent = r.used_percent;
  s.records_used = r.used;
  s.records_capacity = r.capacity;
  s.encounter_records = r.encounter_records;
}

// ---- Captive portal ---------------------------------------------------------------------

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

esp_err_t redirect_to_dashboard(httpd_req_t *req, const char *local_ip) {
  char location[32];
  snprintf(location, sizeof(location), "http://%s/", local_ip[0] ? local_ip : "192.168.4.1");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}

// Redirects requests for other hosts, so phones treat the network as a
// captive portal and open the dashboard in their sign-in window.
bool redirect_if_needed(httpd_req_t *req) {
  char local_ip[16];
  local_address(req, local_ip, sizeof(local_ip));
  char host[64] = "";
  const esp_err_t err = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) snprintf(host, sizeof(host), "?");
  if (fy::portal::route_request(host, req->uri, local_ip, CONFIG_FY_DASHBOARD_HOST) ==
      fy::portal::Route::Page) {
    return false;
  }
  redirect_to_dashboard(req, local_ip);
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

// True when the request was answered with a redirect to the dashboard.
bool portal_redirected(httpd_req_t *req) {
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
  return redirect_if_needed(req);
#else
  (void)req;
  return false;
#endif
}

// ---- HTTP helpers -------------------------------------------------------------------------

void no_store(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

esp_err_t see_other(httpd_req_t *req, const char *location) {
  no_store(req);
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", location);
  return httpd_resp_send(req, nullptr, 0);
}

// Login and password forms: no scripts at all.
esp_err_t send_form_page(httpd_req_t *req, size_t len) {
  if (!len) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
  no_store(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
                     "frame-ancestors 'none'");
  return httpd_resp_send(req, page, static_cast<ssize_t>(len));
}

esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *error) {
  no_store(req);
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  char body[48];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
  return httpd_resp_sendstr(req, body);
}

// The session cookie's token, or an empty string.
void session_token(httpd_req_t *req, char *token, size_t cap) {
  token[0] = '\0';
  size_t len = cap;
  if (httpd_req_get_cookie_val(req, kCookie, token, &len) != ESP_OK) token[0] = '\0';
}

// The page marks its own polling and position updates with this header, so
// an open tab keeps neither a Wi-Fi session nor its login alive by itself.
bool background_request(httpd_req_t *req) {
  char value[4];
  return httpd_req_get_hdr_value_str(req, "X-FY-Background", value, sizeof(value)) == ESP_OK;
}

bool logged_in(httpd_req_t *req) {
  char token[fy::dashboard::kTokenLen + 8];
  session_token(req, token, sizeof(token));
  const bool background = background_request(req);
  if (!sessions.touch(token, now_ms(), !background)) return false;
  // A request the owner made keeps a Wi-Fi session open.
  if (request_hook && !background) request_hook();
  return true;
}

// A query parameter as text; empty when absent.
void query_value(httpd_req_t *req, const char *key, char *out, size_t cap) {
  out[0] = '\0';
  char query[96];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, key, out, cap) != ESP_OK) {
    out[0] = '\0';
  }
}

// ---- Notes on addresses -----------------------------------------------------------------

// Loaded from NVS on first use; used only by the HTTP server task.
fy::dashboard::Notes notes;
bool notes_loaded = false;

void load_notes() {
  if (notes_loaded) return;
  notes_loaded = true;
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_blob(nvs, "notes", nullptr, &len) == ESP_OK && len <= fy::dashboard::Notes::kMaxBytes) {
    uint8_t *buf = static_cast<uint8_t *>(malloc(len));
    if (buf && nvs_get_blob(nvs, "notes", buf, &len) == ESP_OK && !notes.deserialize(buf, len)) {
      ESP_LOGW(TAG, "stored notes unreadable; starting empty");
    }
    free(buf);
  }
  nvs_close(nvs);
}

esp_err_t save_notes() {
  uint8_t *buf = static_cast<uint8_t *>(malloc(fy::dashboard::Notes::kMaxBytes));
  if (!buf) return ESP_ERR_NO_MEM;
  const size_t len = notes.serialize(buf, fy::dashboard::Notes::kMaxBytes);
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    err = nvs_set_blob(nvs, "notes", buf, len);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
  }
  free(buf);
  return err;
}

void set_session_cookie(httpd_req_t *req, const char *token) {
  // Kept in a static buffer: httpd sends headers when the response goes out.
  static char cookie[96];
  snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; HttpOnly; SameSite=Strict", kCookie, token);
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

// Rejects form posts made by other sites' pages. Browsers send Origin with
// every POST; a missing or "null" Origin (some sign-in windows) is accepted,
// and the SameSite cookie still applies.
bool same_origin(httpd_req_t *req) {
  char origin[80];
  if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return true;
  if (strcmp(origin, "null") == 0) return true;
  char host[64];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
  return strncmp(origin, "http://", 7) == 0 && strcmp(origin + 7, host) == 0;
}

bool read_body(httpd_req_t *req, char *buf, size_t cap, size_t *len) {
  if (req->content_len >= cap) return false;
  size_t got = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, buf + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  buf[got] = '\0';
  *len = got;
  return true;
}

// Wipes a password from a buffer once it is no longer needed.
void wipe(void *p, size_t n) {
  volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
  while (n--) *v++ = 0;
}

// ---- Pages ---------------------------------------------------------------------------------

esp_err_t send_login(httpd_req_t *req, LoginNotice notice, uint32_t wait_s = 0) {
  return send_form_page(req, fy::dashboard::render_login_page(notice, wait_s, page, sizeof(page)));
}

esp_err_t send_password_page(httpd_req_t *req, const char *problem) {
  return send_form_page(req, fy::dashboard::render_password_page(password_is_default(), problem,
                                                                 page, sizeof(page)));
}

esp_err_t root_get(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!logged_in(req)) return see_other(req, "/login");
  if (password_is_default()) return see_other(req, "/password");
  no_store(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  // Nothing loads from anywhere but the device.
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; "
                     "connect-src 'self'; form-action 'self'; frame-ancestors 'none'");
  return httpd_resp_send(req, reinterpret_cast<const char *>(index_html_start),
                         index_html_end - index_html_start);
}

esp_err_t asset_get(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  const bool js = strncmp(req->uri, "/app.js", 7) == 0;
  httpd_resp_set_type(req, js ? "text/javascript; charset=utf-8" : "text/css; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  const uint8_t *start = js ? app_js_start : app_css_start;
  const uint8_t *end = js ? app_js_end : app_css_end;
  return httpd_resp_send(req, reinterpret_cast<const char *>(start), end - start);
}

esp_err_t login_get(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (logged_in(req)) return see_other(req, "/");
  char query[16] = "";
  char m[8] = "";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "m", m, sizeof(m));
  }
  const LoginNotice notice = strcmp(m, "out") == 0   ? LoginNotice::LoggedOut
                             : strcmp(m, "exp") == 0 ? LoginNotice::Expired
                                                     : LoginNotice::None;
  return send_login(req, notice);
}

esp_err_t login_post(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (request_hook) request_hook();
  if (!same_origin(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request");
  const uint32_t wait = throttle.wait_ms(now_ms());
  if (wait) return send_login(req, LoginNotice::Wait, (wait + 999) / 1000);

  char body[kBodyCap];
  size_t len = 0;
  char password[fy::dashboard::kMaxPasswordLen * 2 + 1];
  const bool ok = read_body(req, body, sizeof(body), &len) &&
                  fy::dashboard::form_value(body, len, "password", password, sizeof(password)) &&
                  verify_password(password);
  wipe(body, sizeof(body));
  wipe(password, sizeof(password));
  if (!ok) {
    throttle.failed(now_ms());
    ESP_LOGW(TAG, "dashboard login failed");
    return send_login(req, LoginNotice::Failed);
  }
  throttle.succeeded();
  char token[fy::dashboard::kTokenLen + 1];
  sessions.create(hardware_random_byte, nullptr, now_ms(), token);
  set_session_cookie(req, token);
  ESP_LOGI(TAG, "dashboard login");
  return see_other(req, password_is_default() ? "/password" : "/");
}

esp_err_t logout_post(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!same_origin(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request");
  char token[fy::dashboard::kTokenLen + 8];
  session_token(req, token, sizeof(token));
  sessions.end(token);
  httpd_resp_set_hdr(req, "Set-Cookie", "fy_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  return see_other(req, "/login?m=out");
}

esp_err_t password_get(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!logged_in(req)) return see_other(req, "/login?m=exp");
  return send_password_page(req, "");
}

esp_err_t password_post(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!logged_in(req)) return see_other(req, "/login?m=exp");
  if (!same_origin(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request");

  char body[kBodyCap];
  size_t len = 0;
  constexpr size_t kField = fy::dashboard::kMaxPasswordLen * 2 + 1;
  char current[kField] = "", fresh[kField] = "", confirm[kField] = "";
  PasswordProblem problem = PasswordProblem::None;
  const bool first_login = password_is_default();
  if (!read_body(req, body, sizeof(body), &len)) {
    problem = PasswordProblem::TooLong;
  } else {
    fy::dashboard::form_value(body, len, "current", current, sizeof(current));
    fy::dashboard::form_value(body, len, "new", fresh, sizeof(fresh));
    fy::dashboard::form_value(body, len, "confirm", confirm, sizeof(confirm));
    problem = fy::dashboard::check_new_password(fresh, confirm, kDefaultPassword);
    // Logged in already proves the default on first login; later changes
    // also need the current password, and wrong ones count as failed logins.
    if (problem == PasswordProblem::None && !first_login) {
      const uint32_t wait = throttle.wait_ms(now_ms());
      if (wait || !verify_password(current)) {
        if (!wait) throttle.failed(now_ms());
        problem = PasswordProblem::WrongCurrent;
      }
    }
  }
  esp_err_t stored = ESP_OK;
  if (problem == PasswordProblem::None) stored = store_password(fresh);
  wipe(body, sizeof(body));
  wipe(current, sizeof(current));
  wipe(fresh, sizeof(fresh));
  wipe(confirm, sizeof(confirm));
  if (problem != PasswordProblem::None) return send_password_page(req, describe(problem));
  if (stored != ESP_OK) return send_password_page(req, "The password could not be saved.");

  // Other browsers logged in with the old password are logged out.
  throttle.succeeded();
  sessions.end_all();
  char token[fy::dashboard::kTokenLen + 1];
  sessions.create(hardware_random_byte, nullptr, now_ms(), token);
  set_session_cookie(req, token);
  return see_other(req, "/");
}

// API and exports: logged in, with the default password replaced.
bool api_allowed(httpd_req_t *req) {
  if (!logged_in(req)) {
    send_json_error(req, "401 Unauthorized", "login");
    return false;
  }
  if (password_is_default()) {
    send_json_error(req, "403 Forbidden", "password");
    return false;
  }
  return true;
}

esp_err_t status_get(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!api_allowed(req)) return ESP_OK;
  fy::dashboard::Status s;
  gather_status(s);
  fy_recorder_log_cursor cursor;
  if (fy_recorder_log_snapshot(&cursor)) s.record_seq = cursor.next_seq;
  s.phone_time = fy_recorder_phone_time_set();
  const size_t len = fy::dashboard::format_status_json(s, page, sizeof(page));
  if (!len) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
  no_store(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, page, static_cast<ssize_t>(len));
}

// ---- Streaming stored encounters -----------------------------------------------------

enum class Format : uint8_t { Json, Csv };

// Buffers for one streamed response, allocated per request (about 8 KB).
struct Stream {
  uint8_t sector[kFyRecorderSectorBytes];
  fy::store::EncounterDedup dedup;
  char line[fy::dashboard::kEncounterTextCap];
  char chunk[2048];
  char next_seq[12];  // header value; sent with the first chunk
  size_t used = 0;
  bool failed = false;  // the client went away
};

void stream_flush(httpd_req_t *req, Stream &s) {
  if (s.failed || s.used == 0) return;
  if (httpd_resp_send_chunk(req, s.chunk, static_cast<ssize_t>(s.used)) != ESP_OK) s.failed = true;
  s.used = 0;
}

void stream_add(httpd_req_t *req, Stream &s, const char *text, size_t len) {
  if (s.used + len > sizeof(s.chunk)) stream_flush(req, s);
  if (s.failed || len > sizeof(s.chunk)) return;
  memcpy(s.chunk + s.used, text, len);
  s.used += len;
}

// Every stored encounter, current snapshot only, newest first, with the
// owner's notes. `?since=N` limits it to encounters updated in records N and
// later (the page's live updates); the X-FY-Next-Seq header gives the N for
// the next request. Downloads leave out ignored addresses unless
// `?ignored=1`.
esp_err_t stream_encounters(httpd_req_t *req, Format format, bool download) {
  if (portal_redirected(req)) return ESP_OK;
  if (!api_allowed(req)) return ESP_OK;
  char arg[16];
  query_value(req, "since", arg, sizeof(arg));
  const uint32_t since = static_cast<uint32_t>(strtoul(arg, nullptr, 10));
  query_value(req, "ignored", arg, sizeof(arg));
  const bool skip_ignored = download && strcmp(arg, "1") != 0;
  load_notes();
  Stream *s = new (std::nothrow) Stream;
  if (!s) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");

  fy_recorder_log_cursor cursor;
  const bool have_log = fy_recorder_log_snapshot(&cursor);
  snprintf(s->next_seq, sizeof(s->next_seq), "%lu",
           have_log ? static_cast<unsigned long>(cursor.next_seq) : 0ul);
  no_store(req);
  httpd_resp_set_hdr(req, "X-FY-Next-Seq", s->next_seq);
  httpd_resp_set_type(req, format == Format::Json ? "application/json" : "text/csv; charset=utf-8");
  if (download) {
    httpd_resp_set_hdr(req, "Content-Disposition",
                       format == Format::Json
                           ? "attachment; filename=\"detector-detections.json\""
                           : "attachment; filename=\"detector-detections.csv\"");
  }
  if (format == Format::Json) {
    stream_add(req, *s, "[", 1);
  } else {
    const char *header = fy::dashboard::encounter_csv_header();
    stream_add(req, *s, header, strlen(header));
  }

  size_t count = 0;
  bool older = false;  // reached records before `since`
  if (have_log) {
    for (uint32_t i = 0; i < cursor.sectors && !s->failed && !older; i++) {
      const uint32_t sector = (cursor.newest_sector + cursor.sectors - i) % cursor.sectors;
      const size_t n = fy_recorder_read_sector(sector, s->sector);
      for (size_t j = n; j-- > 0 && !s->failed;) {
        const uint8_t *raw = s->sector + j * fy::store::kRecordSize;
        fy::store::RecordHeader h;
        memcpy(&h, raw, sizeof(h));
        if (h.seq >= cursor.next_seq) continue;  // written after the snapshot
        if (h.seq < since) {  // every record from here on is older still
          older = true;
          break;
        }
        if (h.type != fy::store::kTypeEncounter) continue;
        fy::store::EncounterRecord rec;
        memcpy(&rec, raw, sizeof(rec));
        if (!s->dedup.keep(rec)) continue;
        const fy::dashboard::Note *note = notes.find(rec.mac);
        if (skip_ignored && note && note->ignored) continue;
        const size_t len = format == Format::Json
                               ? fy::dashboard::encounter_json(rec, note, s->line, sizeof(s->line))
                               : fy::dashboard::encounter_csv(rec, note, s->line, sizeof(s->line));
        if (!len) continue;
        if (format == Format::Json) stream_add(req, *s, count ? ",\n" : "\n", count ? 2 : 1);
        stream_add(req, *s, s->line, len);
        count++;
      }
    }
  }
  if (format == Format::Json) stream_add(req, *s, "\n]\n", 3);
  stream_flush(req, *s);
  const bool failed = s->failed;
  delete s;
  if (failed) return ESP_FAIL;  // closes the connection
  if (since == 0) ESP_LOGI(TAG, "sent %u encounters", static_cast<unsigned>(count));
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t detections_get(httpd_req_t *req) { return stream_encounters(req, Format::Json, false); }
esp_err_t export_json_get(httpd_req_t *req) { return stream_encounters(req, Format::Json, true); }
esp_err_t export_csv_get(httpd_req_t *req) { return stream_encounters(req, Format::Csv, true); }

esp_err_t send_json(httpd_req_t *req, const char *json, size_t len) {
  no_store(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, static_cast<ssize_t>(len));
}

// The browser's clock (t, Unix ms), for detections without GNSS time.
esp_err_t phone_post(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!same_origin(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request");
  if (!api_allowed(req)) return ESP_OK;
  char body[kBodyCap];
  size_t len = 0;
  if (!read_body(req, body, sizeof(body), &len)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
  }
  char t[24];
  if (fy::dashboard::form_value(body, len, "t", t, sizeof(t))) {
    fy_recorder_set_phone_time(strtoull(t, nullptr, 10));
  }
  return send_json(req, "{}", 2);
}

// Every note, streamed one at a time.
esp_err_t notes_get(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!api_allowed(req)) return ESP_OK;
  load_notes();
  no_store(req);
  httpd_resp_set_type(req, "application/json");
  char one[fy::dashboard::kNoteJsonCap + 1];
  esp_err_t err = httpd_resp_send_chunk(req, "[", 1);
  for (size_t i = 0; i < notes.count() && err == ESP_OK; i++) {
    one[0] = ',';
    const size_t len = fy::dashboard::note_json(notes.at(i), one + 1, sizeof(one) - 1);
    if (len) err = httpd_resp_send_chunk(req, i ? one : one + 1, static_cast<ssize_t>(i ? len + 1 : len));
  }
  if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]", 1);
  if (err != ESP_OK) return ESP_FAIL;  // closes the connection
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Sets or clears the note on one address: mac, label, ignored ("1"), note.
esp_err_t notes_post(httpd_req_t *req) {
  if (portal_redirected(req)) return ESP_OK;
  if (!same_origin(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request");
  if (!api_allowed(req)) return ESP_OK;
  char body[kBodyCap];
  size_t len = 0;
  char mac[24], label[24], ignored[4], text[fy::dashboard::kNoteTextCap * 3];
  fy::dashboard::Note note = {};
  if (!read_body(req, body, sizeof(body), &len) ||
      !fy::dashboard::form_value(body, len, "mac", mac, sizeof(mac)) ||
      !fy::dashboard::parse_mac(mac, note.mac)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad address");
  }
  if (fy::dashboard::form_value(body, len, "label", label, sizeof(label)) &&
      !fy::dashboard::parse_label(label, &note.label)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad label");
  }
  note.ignored = fy::dashboard::form_value(body, len, "ignored", ignored, sizeof(ignored)) &&
                 strcmp(ignored, "1") == 0;
  if (fy::dashboard::form_value(body, len, "note", text, sizeof(text))) {
    fy::dashboard::clean_note_text(text, note.text);
  }
  load_notes();
  if (!notes.set(note)) return send_json_error(req, "409 Conflict", "notes full");
  if (save_notes() != ESP_OK) return send_json_error(req, "500 Internal Server Error", "not saved");
  return send_json(req, "{}", 2);
}

// Anything else: the portal sends it to the dashboard; otherwise not found.
esp_err_t other_get(httpd_req_t *req) {
#if CONFIG_FY_DASHBOARD_CAPTIVE_PORTAL
  char local_ip[16];
  local_address(req, local_ip, sizeof(local_ip));
  return redirect_to_dashboard(req, local_ip);
#else
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
#endif
}

// ---- Server ------------------------------------------------------------------------------

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
  struct Route {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
  };
  // Matched in order, so the catch-all comes last.
  static const Route kRoutes[] = {
      {"/", HTTP_GET, root_get},
      {"/app.js", HTTP_GET, asset_get},
      {"/app.css", HTTP_GET, asset_get},
      {"/login", HTTP_GET, login_get},
      {"/login", HTTP_POST, login_post},
      {"/logout", HTTP_POST, logout_post},
      {"/password", HTTP_GET, password_get},
      {"/password", HTTP_POST, password_post},
      {"/api/status", HTTP_GET, status_get},
      {"/api/detections", HTTP_GET, detections_get},
      {"/api/phone", HTTP_POST, phone_post},
      {"/api/notes", HTTP_GET, notes_get},
      {"/api/notes", HTTP_POST, notes_post},
      {"/export/detections.json", HTTP_GET, export_json_get},
      {"/export/detections.csv", HTTP_GET, export_csv_get},
      {"/*", HTTP_GET, other_get},
  };
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;  // password hashing
  config.max_open_sockets = 4;
  config.max_uri_handlers = sizeof(kRoutes) / sizeof(kRoutes[0]);
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");
  esp_err_t err = ESP_OK;
  for (const Route &r : kRoutes) {
    httpd_uri_t uri = {};
    uri.uri = r.uri;
    uri.method = r.method;
    uri.handler = r.handler;
    err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) break;
  }
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
  ESP_LOGI(TAG, "access point %s on channel %d, dashboard http://%s/", ssid,
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
