/*
 * Temporary MKR WiFi 1010 diagnostic. No BMS timer, FlashStorage, Wi-Fi
 * credentials, or API requests. Heater pin 4 stays HIGH throughout this test.
 * Restore BMSController and configure it again when the test is complete.
 */
#include <Arduino.h>
#include <SPI.h>
#include <WiFiNINA.h>
#include <ArduinoBLE.h>
#include <Arduino_SpiNINA.h>

#if !defined(ARDUINO_SAMD_MKRWIFI1010)
#error "Select Arduino MKR WiFi 1010 (arduino:samd:mkrwifi1010)."
#endif

bool bleStarted = false;
String line;

bool probeFirmware() {
  Serial.println("Checking WiFi.status()...");
  const uint8_t status = WiFi.status();
  Serial.print("WiFi status: "); Serial.println(status);
  Serial.print("WL_NO_MODULE: "); Serial.println(WL_NO_MODULE);
  Serial.println("Reading NINA firmware...");
  const String version = WiFi.firmwareVersion();
  Serial.print("NINA firmware: ["); Serial.print(version); Serial.println(']');
  Serial.print("Version length: "); Serial.println(version.length());
  if (!version.length() || status == WL_NO_MODULE) {
    Serial.println("NINA probe failed without BMS timer or storage code.");
    Serial.println("Type recover to test the shared driver's NINA reset, or probe to repeat the read.");
    return false;
  }
  Serial.println("NINA firmware response received.");
  return true;
}

void help() {
  Serial.println("Commands: probe, recover, ble, help");
  Serial.println("probe: read Wi-Fi status and NINA version without BMS code.");
  Serial.println("recover: reset NINA using the shared SPI driver, wait 1.5 seconds, and repeat probe.");
  Serial.println("ble: enable HCI tracing, initialize Bluetooth, and start scanning.");
  Serial.println("Heater pin 4 remains HIGH. This diagnostic does not regulate heating.");
}

void setup() {
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);
  Serial.begin(115200);
  // Waiting is intentional only in this temporary diagnostic, so its output is captured.
  while (!Serial) { delay(10); }
  line.reserve(32);
  Serial.println("MKR WiFi 1010 standalone NINA probe; no BMS timer or storage.");
  help();
  probeFirmware();
}

void loop() {
  if (bleStarted) BLE.poll();
  while (Serial.available()) {
    const char character = Serial.read();
    if (character == '\r') continue;
    if (character != '\n') {
      if (line.length() < 32) line += character;
      continue;
    }
    line.trim();
    if (line == "probe") probeFirmware();
    else if (line == "recover") {
      Serial.println("Resetting only the NINA module...");
      SpiDrv::begin(true);
      bleStarted = false;
      delay(1500);
      probeFirmware();
    } else if (line == "ble") {
      if (bleStarted) Serial.println("BLE scanning is already active.");
      else if (probeFirmware()) {
        BLE.debug(Serial);
        Serial.println("Calling BLE.begin()...");
        if (!BLE.begin()) Serial.println("BLE.begin() failed in the standalone probe.");
        else {
          Serial.println("BLE.begin() succeeded.");
          bleStarted = BLE.scan(true);
          Serial.println(bleStarted ? "BLE scanning started." : "BLE scan failed.");
          if (!bleStarted) BLE.end();
        }
      }
    } else if (line == "help") help();
    else Serial.println("Unknown command. Type help.");
    line = "";
  }
}
