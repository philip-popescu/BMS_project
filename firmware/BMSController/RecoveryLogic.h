#pragma once
#include <stdint.h>

// Written under the interrupt lock; evaluated by TC5 even if NINA SPI stalls.
struct RecoveryLogic {
  static const uint32_t TIMEOUT_MS = 600000UL;
  bool enabled = false;
  bool advertisementSeen = false;
  bool restartPending = false;
  uint32_t wifiStamp = 0;
  uint32_t advertisementStamp = 0;
  uint32_t restartStamp = 0;
  uint32_t restartDelay = 0;

  void arm(uint32_t now) volatile {
    enabled = true;
    wifiStamp = now;
    advertisementStamp = now;
  }
  void suspend() volatile { enabled = false; }
  void wifi(bool connected, uint32_t now) volatile {
    if (connected) wifiStamp = now;
  }
  void advertisement(uint32_t now) volatile {
    advertisementStamp = now;
    advertisementSeen = true;
  }
  void schedule(uint32_t now, uint32_t delay) volatile {
    restartStamp = now;
    restartDelay = delay;
    restartPending = true;
  }
  bool due(uint32_t now) const volatile {
    if (restartPending && uint32_t(now - restartStamp) >= restartDelay) return true;
    return enabled && (uint32_t(now - wifiStamp) > TIMEOUT_MS ||
                       uint32_t(now - advertisementStamp) > TIMEOUT_MS);
  }
};
