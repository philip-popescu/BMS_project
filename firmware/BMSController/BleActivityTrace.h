#pragma once
#include <stdint.h>

// ArduinoBLE 2.1 emits completed HCI events through its public debug stream.
// Count raw advertising reports, including ones that never produce BLEDiscovered.
// Retain only the first five bytes; no packet buffers or serial output are needed.
struct BleActivityTrace {
  uint8_t prefixPosition = 0, first[5] = {}, highNibble = 0;
  uint16_t bytes = 0;
  bool valid = true, halfByte = false;

  void reset() { prefixPosition = highNibble = 0; bytes = 0; valid = true; halfByte = false; }
  bool feed(char value) {
    static const char prefix[] = "HCI EVENT RX <- ";
    if (value == '\n') {
      const bool report = valid && prefixPosition == sizeof(prefix) - 1 && !halfByte && bytes >= 5 &&
        first[0] == 0x04 && first[1] == 0x3e && bytes == uint16_t(first[2]) + 3 &&
        ((first[3] == 0x02 && first[2] >= 12) || (first[3] == 0x0d && first[2] >= 26)) && first[4] > 0;
      reset();
      return report;
    }
    if (value == '\r' || !valid) return false;
    if (prefixPosition < sizeof(prefix) - 1) {
      if (value == prefix[prefixPosition]) ++prefixPosition;
      else valid = false;
      return false;
    }
    const int nibble = value >= '0' && value <= '9' ? value - '0' : value >= 'A' && value <= 'F' ? value - 'A' + 10 :
      value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
    if (nibble < 0 || bytes > 258) { valid = false; return false; }
    if (!halfByte) { highNibble = nibble; halfByte = true; }
    else {
      if (bytes < sizeof(first)) first[bytes] = uint8_t((highNibble << 4) | nibble);
      ++bytes; halfByte = false;
    }
    return false;
  }
};
