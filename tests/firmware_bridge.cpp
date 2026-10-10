#include "../firmware/BMSController/ControlLogic.h"
#include "../firmware/BMSController/NetworkLogic.h"
#include "../firmware/BMSController/Ruuvi.h"

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
