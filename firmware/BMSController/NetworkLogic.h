#pragma once
#include <stdint.h>

namespace BmsNetwork {

inline uint32_t ipv4Value(const uint8_t address[4]) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4; ++i) value = (value << 8) | address[i];
  return value;
}

inline bool validSubnetMask(const uint8_t address[4]) {
  const uint32_t mask = ipv4Value(address);
  const uint32_t inverse = ~mask;
  // Static LAN configuration needs contiguous mask bits and usable hosts.
  return mask && inverse > 1 && (inverse & (inverse + 1)) == 0;
}

inline bool validAddress(const uint8_t address[4], bool subnetMask) {
  if (subnetMask) return validSubnetMask(address);
  return ipv4Value(address) != 0 && address[0] < 255;
}

inline bool validStaticNetwork(const uint8_t address[4], const uint8_t subnet[4],
                               const uint8_t gateway[4]) {
  if (!validSubnetMask(subnet)) return false;
  const uint32_t mask = ipv4Value(subnet);
  const uint32_t inverse = ~mask;
  const uint32_t ip = ipv4Value(address);
  return (ip & mask) == (ipv4Value(gateway) & mask) &&
    (ip & inverse) != 0 && (ip & inverse) != inverse;
}

}  // namespace BmsNetwork
