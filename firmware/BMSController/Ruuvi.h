#pragma once
#include <stdint.h>
#include <stddef.h>

inline bool decodeRuuvi(const uint8_t* manufacturer, size_t length, double& temperature) {
  // Company ID + complete 24-byte RAWv2 payload. Never use fixed AD offsets.
  if (length < 26 || manufacturer[0] != 0x99 || manufacturer[1] != 0x04 || manufacturer[2] != 5) return false;
  const uint16_t bits = (uint16_t(manufacturer[3]) << 8) | manufacturer[4];
  if (bits == 0x8000) return false;  // RAWv2 unavailable-temperature sentinel.
  const int32_t signedValue = bits >= 0x8000 ? int32_t(bits) - 65536 : bits;
  temperature = signedValue * 0.005;
  return true;
}
