// SPDX-License-Identifier: MIT

#include "fy_dashboard_json.h"

#include <cstdio>

namespace fy {
namespace {

unsigned frequency_mhz(uint8_t channel) {
  if (channel >= 1 && channel <= 13) return 2407 + 5 * channel;
  if (channel == 14) return 2484;
  return 0;
}

}  // namespace

void sanitize_name(const char *in, char *out, size_t cap) {
  if (cap == 0) return;
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < cap; i++) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    out[o++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? static_cast<char>(c) : '?';
  }
  out[o] = '\0';
}

size_t format_dashboard_detection(const Match &m, int rssi, uint8_t channel, char *out,
                                  size_t cap) {
  if (!out || cap == 0) return 0;
  const RuleInfo &info = rule_info(m.rule);
  const bool ble = info.transport == Transport::Ble;
  if (ble) channel = kDashboardBleChannel;
  char name[sizeof(m.name)];
  sanitize_name(m.name, name, sizeof(name));

  const int n = snprintf(
      out, cap,
      "{\"event\":\"detection\",\"detection_method\":\"%s\",\"detection_tier\":%u,"
      "\"protocol\":\"%s\",\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
      "\"oui\":\"%02x:%02x:%02x\",\"device_name\":\"%s\",\"rssi\":%d,\"channel\":%u,"
      "\"frequency\":%u,\"ssid\":\"\",\"rule\":\"%s\",\"strength\":\"%s\"}",
      info.upstream_method, static_cast<unsigned>(info.upstream_tier),
      ble ? "ble" : "wifi_2_4ghz", m.mac[0], m.mac[1], m.mac[2], m.mac[3], m.mac[4], m.mac[5],
      m.mac[0], m.mac[1], m.mac[2], name, rssi, static_cast<unsigned>(channel),
      ble ? 0u : frequency_mhz(channel), info.id,
      info.strength == Strength::Specific ? "specific" : "possible");
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

}  // namespace fy
