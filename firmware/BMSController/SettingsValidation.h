#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include "NetworkLogic.h"

// Retain the original FlashStorage layout, including its checksum location.
struct Configuration {
  uint32_t magic;
  char ssid[33];
  char password[65];
  char apiHost[128];
  uint16_t apiPort;
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];
  uint8_t dns[4];
  char sensorMac[18];
  uint8_t ipConfigured;
  uint32_t checksum;
};

namespace BmsSettings {
inline bool terminated(const char* value, size_t size) { return memchr(value, 0, size) != NULL; }
inline bool printable(const char* value) {
  for (; *value; ++value) if (static_cast<unsigned char>(*value) < 32 || *value == 127) return false;
  return true;
}
inline bool validMac(const char* value, bool allowEmpty = false) {
  if (!value) return false;
  if (!*value) return allowEmpty;
  if (strlen(value) != 17) return false;
  for (size_t i = 0; i < 17; ++i) {
    if (i % 3 == 2) { if (value[i] != ':') return false; }
    else if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}
inline bool validHost(const char* value) {
  if (!value || !*value || strlen(value) > 127) return false;
  for (; *value; ++value) if (!isalnum(static_cast<unsigned char>(*value)) && *value != '.' && *value != '-') return false;
  return true;
}
inline bool validPassword(const char* value) {
  const size_t length = strlen(value);
  if (length && (length < 8 || length > 63)) return false;
  for (; *value; ++value) if (*value < 32 || *value > 126) return false;
  return true;
}
inline bool hostAddress(const uint8_t address[4]) {
  return address[0] > 0 && address[0] < 224 && address[0] != 127;
}
inline bool validNetwork(const Configuration& value) {
  if (!hostAddress(value.ip) || !hostAddress(value.gateway) || !hostAddress(value.dns) ||
      !BmsNetwork::validStaticNetwork(value.ip, value.subnet, value.gateway)) return false;
  const uint32_t hostMask = ~BmsNetwork::ipv4Value(value.subnet);
  const uint32_t gatewayHost = BmsNetwork::ipv4Value(value.gateway) & hostMask;
  return gatewayHost && gatewayHost != hostMask && memcmp(value.ip, value.gateway, 4) != 0;
}
inline bool valid(const Configuration& value, bool requireSsid = false) {
  if (!terminated(value.ssid, sizeof(value.ssid)) || !terminated(value.password, sizeof(value.password)) ||
      !terminated(value.apiHost, sizeof(value.apiHost)) || !terminated(value.sensorMac, sizeof(value.sensorMac))) return false;
  return (!requireSsid || *value.ssid) && printable(value.ssid) && validPassword(value.password) &&
    validHost(value.apiHost) && value.apiPort && validMac(value.sensorMac, true) &&
    value.ipConfigured == 1 && validNetwork(value);
}
inline bool validCommandId(const char* value) {
  if (!value || strlen(value) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
    else if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}
inline bool parseIpv4(const char* text, uint8_t out[4]) {
  if (!text) return false;
  uint8_t candidate[4];
  for (size_t part = 0; part < 4; ++part) {
    unsigned value = 0, digits = 0;
    const bool leadingZero = *text == '0';
    while (*text >= '0' && *text <= '9') {
      value = value * 10 + (*text++ - '0');
      if (++digits > 3 || value > 255) return false;
    }
    if (!digits || (leadingZero && digits > 1) || (part < 3 ? *text++ != '.' : *text != 0)) return false;
    candidate[part] = value;
  }
  memcpy(out, candidate, 4);
  return true;
}
}  // namespace BmsSettings
