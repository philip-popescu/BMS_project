#include "../firmware/BMSController/ControlLogic.h"
#include "../firmware/BMSController/NetworkLogic.h"
#include "../firmware/BMSController/Ruuvi.h"
#include "../firmware/BMSController/RecoveryLogic.h"
#include "../firmware/BMSController/SettingsValidation.h"
#include "../firmware/BMSController/ManagementHttp.h"
#include "../firmware/BMSController/BleActivityTrace.h"

#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

static ControlLogic state;
EXPORT void logic_reset() { state = ControlLogic(); }
EXPORT void logic_reading(double value, uint32_t now) { state.reading(value, now); }
EXPORT void logic_sensor_changed(uint32_t now) { state.sensorChanged(now); }
EXPORT void logic_api(double target, double negative, double positive, uint32_t now) { state.apiState(target, negative, positive, now); }
EXPORT int logic_tick(uint32_t now) { state.tick(now); return state.heaterOn; }
EXPORT double logic_target() { return state.target; }
EXPORT double logic_negative() { return state.negativeDelta; }
EXPORT double logic_positive() { return state.positiveDelta; }
EXPORT int ruuvi_decode(const uint8_t* payload, size_t length, double* temperature) { return decodeRuuvi(payload, length, *temperature); }
EXPORT int network_address_valid(const uint8_t* address, int subnetMask) {
  return BmsNetwork::validAddress(address, subnetMask != 0);
}
EXPORT int network_config_valid(const uint8_t* address, const uint8_t* subnet, const uint8_t* gateway) {
  return BmsNetwork::validStaticNetwork(address, subnet, gateway);
}

static RecoveryLogic recovery;
EXPORT void recovery_reset(uint32_t now, int provisioning) {
  recovery = RecoveryLogic();
  if (!provisioning) recovery.arm(now);
}
EXPORT void recovery_wifi(int connected, uint32_t now) { recovery.wifi(connected != 0, now); }
EXPORT void recovery_advertisement(uint32_t now) { recovery.advertisement(now); }
EXPORT void recovery_suspend() { recovery.suspend(); }
EXPORT void recovery_arm(uint32_t now) { recovery.arm(now); }
EXPORT void recovery_schedule(uint32_t now, uint32_t delay) { recovery.schedule(now, delay); }
EXPORT int recovery_due(uint32_t now) { return recovery.due(now); }

EXPORT int settings_valid(const char* ssid, const char* password, const char* host, int port, const char* mac,
                          const char* ip, const char* mask, const char* gateway, const char* dns) {
  Configuration config = {};
  if (strlen(ssid) >= sizeof(config.ssid) || strlen(password) >= sizeof(config.password) ||
      strlen(host) >= sizeof(config.apiHost) || strlen(mac) >= sizeof(config.sensorMac) || port <= 0 || port > 65535) return false;
  strcpy(config.ssid, ssid); strcpy(config.password, password); strcpy(config.apiHost, host); strcpy(config.sensorMac, mac);
  config.apiPort = port;
  config.ipConfigured = 1;
  if (!BmsSettings::parseIpv4(ip, config.ip) || !BmsSettings::parseIpv4(mask, config.subnet) ||
      !BmsSettings::parseIpv4(gateway, config.gateway) || !BmsSettings::parseIpv4(dns, config.dns)) return false;
  return BmsSettings::valid(config, true);
}
EXPORT int settings_command_id(const char* value) { return BmsSettings::validCommandId(value); }

static ManagementHttp http;
EXPORT void http_reset() { http.reset(); }
EXPORT int http_feed(const char* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i) http.feed(bytes[i]);
  return http.error ? -http.error : http.complete ? 1 : 0;
}
EXPORT const char* http_method() { return http.method; }
EXPORT const char* http_path() { return http.path; }
EXPORT const char* http_body() { return http.body(); }

static BleActivityTrace trace;
EXPORT void trace_reset() { trace.reset(); }
EXPORT int trace_feed(const char* bytes, size_t size) {
  int reports = 0;
  for (size_t i = 0; i < size; ++i) if (trace.feed(bytes[i])) ++reports;
  return reports;
}
