#pragma once
#include <stdint.h>
#include <math.h>

// This exact logic is also compiled natively for deadline and rollover tests.
struct ControlLogic {
  static const uint32_t SENSOR_TIMEOUT_MS = 30000UL;
  static const uint32_t API_TIMEOUT_MS = 18000000UL;
  double target = 23.0;
  double negativeDelta = 0.1;
  double positiveDelta = 0.1;
  double temperature = 0.0;
  uint32_t sensorStamp = 0;
  uint32_t apiStamp = 0;
  bool hasReading = false;
  bool hasApiState = false;
  bool heaterOn = true;

  void reading(double value, uint32_t now) volatile {
    if (!isfinite(value)) return;
    temperature = value;
    sensorStamp = now;
    hasReading = true;
  }

  void sensorChanged(uint32_t now) volatile {
    hasReading = false;
    sensorStamp = now;
  }

  void apiState(double setpoint, double negative, double positive, uint32_t now) volatile {
    target = setpoint;
    negativeDelta = negative;
    positiveDelta = positive;
    apiStamp = now;
    hasApiState = true;
  }

  void tick(uint32_t now) volatile {
    if (hasApiState && uint32_t(now - apiStamp) >= API_TIMEOUT_MS) {
      target = 23.0;
      negativeDelta = 0.1;
      positiveDelta = 0.1;
      hasApiState = false;
    }
    if (uint32_t(now - sensorStamp) > SENSOR_TIMEOUT_MS) {
      hasReading = false;  // A full millis() cycle must never revive an expired sample.
      heaterOn = true;
      return;
    }
    if (!hasReading) return;
    if (temperature < target - negativeDelta) heaterOn = true;
    else if (temperature > target + positiveDelta) heaterOn = false;
  }
};
