/*
 * Arduino MKR WiFi 1010 / SAMD21. NINA firmware >= 3.0.1,
 * WiFiNINA >= 2.0.0, ArduinoBLE >= 2.0.0, ArduinoJson 7, FlashStorage.
 * TC5 runs heater control at 10 Hz independently of blocking radio calls.
 */
#include <Arduino.h>
#include <ArduinoBLE.h>
#include <WiFiNINA.h>
#include <Arduino_SpiNINA.h>
#include <ArduinoJson.h>
#include <FlashStorage.h>
#include <stddef.h>
#include "ControlLogic.h"
#include "NetworkLogic.h"
#include "Ruuvi.h"

#if !defined(ARDUINO_SAMD_MKRWIFI1010)
#error "Select Arduino MKR WiFi 1010 (arduino:samd:mkrwifi1010)."
#endif

const uint8_t HEATER_PIN = 4;
const uint32_t CONFIG_MAGIC = 0x424D5301;
const uint32_t POLL_INTERVAL_MS = 60000UL;
const uint32_t NINA_STARTUP_WAIT_MS = 1500UL;
const uint32_t SENSOR_LOG_INTERVAL_MS = 1000UL;

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

FlashStorage(configurationStorage, Configuration);
Configuration configuration;
Configuration staged;
volatile ControlLogic control;
bool bleReady = false;
bool radioCompatible = false;
bool ninaVersionRead = false;
uint8_t ninaWifiStatus = WL_NO_MODULE;
String ninaFirmware = "unknown";
bool networkNeedsRestart = false;
uint32_t lastPoll = 0;
uint32_t lastBleAttempt = 0;
uint32_t lastSensorLog = 0;
bool sensorReadingReceived = false;
String serialLine;
String command;
uint8_t promptStep = 0;
bool lineOverflow = false;

uint32_t checksum(const Configuration& value) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&value);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(Configuration, checksum); ++i) hash = (hash ^ data[i]) * 16777619UL;
  return hash;
}

Configuration defaults() {
  Configuration value;
  memset(&value, 0, sizeof(value));
  value.magic = CONFIG_MAGIC;
  strcpy(value.apiHost, "BMS");
  value.apiPort = 2026;
  value.checksum = checksum(value);
  return value;
}

bool validMac(const String& mac) {
  if (mac.length() != 17) return false;
  for (uint8_t i = 0; i < 17; ++i) {
    if (i % 3 == 2) { if (mac[i] != ':') return false; }
    else if (!isxdigit(static_cast<unsigned char>(mac[i]))) return false;
  }
  return true;
}

bool storedValid(const Configuration& value) {
  return value.magic == CONFIG_MAGIC && value.checksum == checksum(value) &&
    memchr(value.ssid, 0, sizeof(value.ssid)) && memchr(value.password, 0, sizeof(value.password)) &&
    memchr(value.apiHost, 0, sizeof(value.apiHost)) && memchr(value.sensorMac, 0, sizeof(value.sensorMac)) &&
    value.apiPort > 0 && strlen(value.apiHost) > 0 && (strlen(value.sensorMac) == 0 || validMac(String(value.sensorMac)));
}

void persist() {
  configuration.magic = CONFIG_MAGIC;
  configuration.checksum = checksum(configuration);
  configurationStorage.write(configuration);
}

// No SPI, heap allocation, or serial output is allowed in this interrupt.
void TC5_Handler() {
  if (TC5->COUNT16.INTFLAG.bit.MC0) {
    TC5->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
    control.tick(millis());
    digitalWrite(HEATER_PIN, control.heaterOn ? HIGH : LOW);
  }
}

void startControlTimer() {
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCM_TC4_TC5) | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  TC5->COUNT16.CTRLA.reg = TC_CTRLA_SWRST;
  while (TC5->COUNT16.STATUS.bit.SYNCBUSY || TC5->COUNT16.CTRLA.bit.SWRST) {}
  TC5->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_WAVEGEN_MFRQ | TC_CTRLA_PRESCALER_DIV1024;
  TC5->COUNT16.CC[0].reg = 4687;
  while (TC5->COUNT16.STATUS.bit.SYNCBUSY) {}
  TC5->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
  NVIC_SetPriority(TC5_IRQn, 3);
  NVIC_ClearPendingIRQ(TC5_IRQn);
  NVIC_EnableIRQ(TC5_IRQn);
  TC5->COUNT16.CTRLA.bit.ENABLE = 1;
  while (TC5->COUNT16.STATUS.bit.SYNCBUSY) {}
}

void blePeripheralDiscoveredHandler(BLEDevice peripheral) {
  String address = peripheral.address();
  address.toUpperCase();
  if (!strlen(configuration.sensorMac) || address != configuration.sensorMac || !peripheral.hasManufacturerData()) return;
  const int length = peripheral.manufacturerDataLength();
  if (length < 26 || length > 31) return;
  uint8_t manufacturer[31];
  peripheral.manufacturerData(manufacturer, length);
  double temperature;
  if (!decodeRuuvi(manufacturer, length, temperature)) return;
  const uint32_t now = millis();
  noInterrupts();
  control.reading(temperature, now);
  interrupts();
  const bool firstReading = !sensorReadingReceived;
  sensorReadingReceived = true;
  // Every valid reading updates control; only serial output is rate-limited.
  if (firstReading || uint32_t(now - lastSensorLog) >= SENSOR_LOG_INTERVAL_MS) {
    lastSensorLog = now;
    if (Serial) {
      Serial.print("Ruuvi sensor "); Serial.print(address);
      Serial.print(" temperature: "); Serial.print(temperature, 3); Serial.println(" C");
    }
  }
}

void readRadioFirmware(bool restartModule) {
  // The standalone probe succeeds with a status request before the version
  // request. Allow NINA to boot without depending on an attached USB monitor.
  if (restartModule || !SpiDrv::initialized) {
    SpiDrv::begin(restartModule);
    delay(NINA_STARTUP_WAIT_MS);
  }
  ninaWifiStatus = WiFi.status();
  ninaFirmware = WiFi.firmwareVersion();
  int major = 0, minor = 0, patch = 0;
  const int parts = sscanf(ninaFirmware.c_str(), "%d.%d.%d", &major, &minor, &patch);
  ninaVersionRead = ninaWifiStatus != WL_NO_MODULE && parts == 3 && major >= 0 && minor >= 0 && patch >= 0;
  radioCompatible = ninaVersionRead && (major > 3 || (major == 3 && (minor > 0 || (minor == 0 && patch >= 1))));
}

void printRadioStatus() {
  Serial.print("NINA firmware: "); Serial.println(ninaFirmware);
  Serial.print("Wi-Fi status at last firmware check: "); Serial.println(ninaWifiStatus);
  Serial.println("BMS requires NINA >= 3.0.1, WiFiNINA >= 2.0.0, and ArduinoBLE >= 2.0.0.");
  Serial.print("Firmware version read: "); Serial.println(ninaVersionRead ? "valid" : "failed");
  Serial.print("Firmware compatibility: "); Serial.println(radioCompatible ? "supported" : "not confirmed");
  Serial.print("BLE scanning: "); Serial.println(bleReady ? "active" : "unavailable");
  if (!ninaVersionRead) {
    Serial.println("Could not confirm NINA communication and firmware over SPI. This does not mean its firmware is outdated.");
    Serial.println("Failed checks trigger a NINA reset; further retries run every 10 seconds. Heating timeout remains active.");
  } else if (!radioCompatible) {
    Serial.println("The detected radio firmware is older than BMS requires. Wi-Fi/BLE initialization is disabled.");
    Serial.println("Arduino IDE: close Serial Monitor, then Tools > Firmware Updater > install NINA >= 3.0.1.");
    Serial.println("Upload BMSController again after the update and re-enter its serial configuration.");
  }
}

const char* wifiStatusText(uint8_t status) {
  switch (status) {
    case WL_CONNECTED: return "connected";
    case WL_IDLE_STATUS: return "idle / waiting for connection";
    case WL_NO_SSID_AVAIL: return "network not found";
    case WL_SCAN_COMPLETED: return "scan complete / not connected";
    case WL_CONNECT_FAILED: return "connection failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    case WL_NO_MODULE: return "NINA unavailable / communication failed";
    case WL_AP_LISTENING: return "access point listening / not joined to Wi-Fi";
    case WL_AP_CONNECTED: return "access point has a client / not joined to Wi-Fi";
    case WL_AP_FAILED: return "access point failed";
    default: return "unknown";
  }
}

void printNetworkStatus() {
  if (ninaVersionRead) {
    // Query the module now: the firmware-check status is a startup snapshot.
    const uint8_t status = WiFi.status();
    Serial.print("Wi-Fi: "); Serial.print(wifiStatusText(status));
    Serial.print(" (status "); Serial.print(status); Serial.println(')');
    Serial.print("Current IP address: ");
    if (status == WL_CONNECTED) Serial.println(WiFi.localIP());
    else Serial.println("unavailable (not connected)");
  } else {
    Serial.println("Wi-Fi: unknown (NINA communication is unconfirmed)");
    Serial.println("Current IP address: unavailable");
  }
  Serial.print("Configured Wi-Fi SSID: ");
  Serial.println(strlen(configuration.ssid) ? configuration.ssid : "not set");
  Serial.print("Configured static IP: ");
  if (configuration.ipConfigured) {
    Serial.println(IPAddress(configuration.ip));
    Serial.print("Configured subnet: "); Serial.println(IPAddress(configuration.subnet));
    Serial.print("Configured gateway: "); Serial.println(IPAddress(configuration.gateway));
  } else Serial.println("not set");
  if (!strlen(configuration.ssid)) Serial.println("Wi-Fi connection requires set-wifi, then reset.");
  if (!configuration.ipConfigured) Serial.println("Wi-Fi connection requires set-ip, then reset.");
  if (networkNeedsRestart) Serial.println("Network changes are pending. Type reset to apply them.");
}

void printControlStatus() {
  // Copy shared fields together; keep formatting and USB output outside the ISR lock.
  noInterrupts();
  const double target = control.target;
  const double negative = control.negativeDelta;
  const double positive = control.positiveDelta;
  const double temperature = control.temperature;
  const bool hasReading = control.hasReading;
  const bool hasApiState = control.hasApiState;
  const uint32_t sensorStamp = control.sensorStamp;
  const uint32_t apiStamp = control.apiStamp;
  interrupts();
  const uint32_t now = millis();
  Serial.print("Active settings (C): target_temperature="); Serial.print(target, 6);
  Serial.print(", negative_delta="); Serial.print(negative, 6);
  Serial.print(", positive_delta="); Serial.println(positive, 6);
  Serial.print("Settings source: ");
  if (hasApiState) {
    Serial.print("API (last valid update "); Serial.print(uint32_t(now - apiStamp)); Serial.println(" ms ago)");
  } else Serial.println("defaults (no active API configuration)");
  Serial.print("Assigned Ruuvi sensor: ");
  Serial.println(strlen(configuration.sensorMac) ? configuration.sensorMac : "not set");
  Serial.print("Latest Ruuvi temperature: ");
  if (sensorReadingReceived) {
    const uint32_t age = uint32_t(now - sensorStamp);
    Serial.print(temperature, 3); Serial.print(" C (age "); Serial.print(age); Serial.print(" ms, ");
    Serial.println(hasReading && age <= ControlLogic::SENSOR_TIMEOUT_MS ? "fresh)" : "stale)");
  } else Serial.println("unavailable (no valid reading from the assigned sensor yet)");
}

void startBle() {
  lastBleAttempt = millis();
  if (bleReady) return;
  // A failed SPI read must not permanently disable a supported module or claim an upgrade is needed.
  if (!ninaVersionRead) {
    Serial.println("Retrying NINA communication after a module reset...");
    // Wi-Fi/API and BLE have not been enabled with an unconfirmed module.
    // Reset NINA only in this failed-probe state; preserve BMS configuration.
    readRadioFirmware(true);
    if (!ninaVersionRead) { printRadioStatus(); return; }
  }
  // Older NINA firmware does not implement the SPI BLE commands used by ArduinoBLE 2.x.
  if (!radioCompatible) return;
  if (!BLE.begin()) {
    Serial.println("BLE initialization failed; heating timeout remains active. Retrying in 10 seconds.");
    printRadioStatus();
    Serial.println("If NINA is already >= 3.0.1, check the selected board and both library versions, then power-cycle the board.");
    return;
  }
  BLE.setEventHandler(BLEDiscovered, blePeripheralDiscoveredHandler);
  bleReady = BLE.scan(true);  // Receive duplicate advertisements to refresh the reading timestamp.
  if (!bleReady) {
    BLE.end();
    Serial.println("BLE initialized but scanning failed. Retrying in 10 seconds.");
  } else Serial.println("BLE scanning started.");
}

void pump() {
  if (bleReady) BLE.poll();
  // Deliberately do not dispatch serial commands inside a partially applied API operation.
  yield();
}

bool applyApiBody(const String& payload) {
  JsonDocument document;
  if (deserializeJson(document, payload)) return false;
  if (!document["target_temperature"].is<double>() || !document["negative_delta"].is<double>() ||
      !document["positive_delta"].is<double>() || !document["sensor_mac"].is<const char*>()) return false;
  const double target = document["target_temperature"].as<double>();
  const double negative = document["negative_delta"].as<double>();
  const double positive = document["positive_delta"].as<double>();
  String mac = document["sensor_mac"].as<const char*>();
  if (!isfinite(target) || !isfinite(negative) || !isfinite(positive) || negative < 0 || positive < 0 || !validMac(mac)) return false;
  mac.toUpperCase();
  const bool changed = mac != configuration.sensorMac;
  const uint32_t now = millis();
  noInterrupts();
  if (changed) control.sensorChanged(now);
  control.apiState(target, negative, positive, now);
  interrupts();
  if (changed) {
    sensorReadingReceived = false;
    mac.toCharArray(configuration.sensorMac, sizeof(configuration.sensorMac));
    persist();
  }
  if (Serial) {
    Serial.print("API config received and applied: ");
    serializeJson(document, Serial);
    Serial.println();
  }
  return true;
}

bool readState(WiFiClient& client) {
  String response;
  response.reserve(1024);
  const uint32_t began = millis();
  int headerEnd = -1;
  int length = -1;
  while (uint32_t(millis() - began) < 5000UL) {
    pump();
    while (client.available()) {
      response += char(client.read());
      if (response.length() > 4096) return false;
      if (headerEnd < 0) {
        headerEnd = response.indexOf("\r\n\r\n");
        if (headerEnd >= 0) {
          String headers = response.substring(0, headerEnd);
          if (!headers.startsWith("HTTP/1.1 200 ") && !headers.startsWith("HTTP/1.0 200 ")) return false;
          headers.toLowerCase();
          int position = headers.indexOf("\r\ncontent-length:");
          if (position < 0 || headers.indexOf("transfer-encoding:") >= 0) return false;
          String count = headers.substring(position + 17);
          count = count.substring(0, count.indexOf('\r') < 0 ? count.length() : count.indexOf('\r'));
          count.trim();
          if (!count.length()) return false;
          for (uint16_t i = 0; i < count.length(); ++i) if (!isdigit(count[i])) return false;
          length = count.toInt();
          if (length <= 0 || length > 1024) return false;
        }
      }
      if (headerEnd >= 0 && int(response.length()) >= headerEnd + 4 + length)
        return applyApiBody(response.substring(headerEnd + 4, headerEnd + 4 + length));
    }
    if (!client.connected() && !client.available()) break;
  }
  return false;
}

void pollApi() {
  if (!radioCompatible || networkNeedsRestart) return;
  if (!strlen(configuration.ssid)) {
    Serial.println("Wi-Fi connection skipped: SSID is not configured. Type set-wifi, then reset.");
    return;
  }
  if (!configuration.ipConfigured) {
    Serial.println("Wi-Fi connection skipped: static network is not configured. Type set-ip, then reset.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Connecting to Wi-Fi: "); Serial.println(configuration.ssid);
    WiFi.config(IPAddress(configuration.ip), IPAddress(configuration.dns), IPAddress(configuration.gateway), IPAddress(configuration.subnet));
    WiFi.setTimeout(3000);
    if (strlen(configuration.password)) WiFi.begin(configuration.ssid, configuration.password);
    else WiFi.begin(configuration.ssid);
    const uint8_t status = WiFi.status();
    if (status != WL_CONNECTED) {
      Serial.print("Wi-Fi connection failed: "); Serial.println(wifiStatusText(status));
      Serial.println("Retrying on next poll. Type status for current connection and saved network settings.");
      return;
    }
    Serial.print("Wi-Fi connected. Current IP address: "); Serial.println(WiFi.localIP());
  }
  WiFiClient client;
  client.setConnectionTimeout(3000);
  if (!client.connect(configuration.apiHost, configuration.apiPort)) {
    Serial.println("API connection failed; last valid settings remain subject to the five-hour timeout.");
    return;
  }
  client.print("GET /get_state HTTP/1.1\r\nHost: ");
  client.print(configuration.apiHost);
  client.print(':');
  client.print(configuration.apiPort);
  client.print("\r\nAccept: application/json\r\nConnection: close\r\n\r\n");
  bool success = readState(client);
  client.stop();
  Serial.println(success ? "API state applied." : "API response rejected or timed out.");
}

void printPrompt() {
  if (command == "set-wifi") Serial.println(promptStep == 0 ? "Enter Wi-Fi SSID:" : "Enter Wi-Fi password (blank for open network):");
  else if (command == "set-api") Serial.println(promptStep == 0 ? "Enter API host (IP or hostname, blank for BMS):" : "Enter API port (blank for 2026):");
  else if (command == "set-sensor") Serial.println("Enter sensor MAC address:");
  else if (command == "set-ip") {
    const char* prompts[] = {"Enter IP address:", "Enter subnet mask (blank for 255.255.255.0):", "Enter gateway IP address:", "Enter DNS IP address (blank to use gateway):"};
    Serial.println(prompts[promptStep]);
  }
}

bool validHost(const String& value) {
  if (!value.length() || value.length() >= sizeof(configuration.apiHost)) return false;
  for (uint16_t i = 0; i < value.length(); ++i) if (!isalnum(value[i]) && value[i] != '.' && value[i] != '-') return false;
  return true;
}

bool parseIp(const String& value, uint8_t output[4], bool subnetMask) {
  IPAddress address;
  if (!address.fromString(value)) return false;
  uint8_t parsed[4];
  for (uint8_t i = 0; i < 4; ++i) parsed[i] = address[i];
  // A subnet mask commonly starts with 255; host-address rules do not apply.
  if (!BmsNetwork::validAddress(parsed, subnetMask)) return false;
  memcpy(output, parsed, sizeof(parsed));
  return true;
}

bool validNetwork(const Configuration& value) {
  return BmsNetwork::validStaticNetwork(value.ip, value.subnet, value.gateway);
}

void handleLine(String value) {
  Serial.println(value);
  if (!command.length()) {
    value.trim();
    if (value == "help") {
      Serial.println("Commands: help, status, debug-radio, set-wifi, set-ip, set-api, set-sensor, reset, factory-reset");
      Serial.println("status: show Wi-Fi/IP, radio/BLE, active settings, and latest Ruuvi temperature and age.");
      Serial.println("debug-radio: enable BLE command tracing and retry a failed radio initialization; reset disables tracing.");
      Serial.println("reset: restart and preserve settings. factory-reset: clear local settings and restart.");
      Serial.println("Use cancel to abandon a prompted update. Network changes require reset.");
      return;
    }
    if (value == "status") { printRadioStatus(); printNetworkStatus(); printControlStatus(); return; }
    if (value == "debug-radio") {
      BLE.debug(Serial);
      Serial.println("BLE command tracing enabled until restart. No configuration was erased.");
      if (!bleReady) { readRadioFirmware(false); startBle(); }
      printRadioStatus();
      return;
    }
    if (value == "reset") { Serial.println("Restarting; stored configuration preserved."); delay(25); NVIC_SystemReset(); }
    if (value == "factory-reset") {
      digitalWrite(HEATER_PIN, HIGH);
      configuration = defaults(); persist();
      Serial.println("Factory configuration restored. Restarting."); delay(25); NVIC_SystemReset();
    }
    if (value != "set-wifi" && value != "set-ip" && value != "set-api" && value != "set-sensor") { Serial.println("Unknown command. Type help."); return; }
    command = value; staged = configuration; promptStep = 0; printPrompt(); return;
  }
  if (value == "cancel") { command = ""; Serial.println("Update cancelled; existing configuration preserved."); return; }
  bool finished = false, accepted = false;
  if (command == "set-wifi") {
    if (promptStep == 0 && value.length() > 0 && value.length() < sizeof(staged.ssid)) { value.toCharArray(staged.ssid, sizeof(staged.ssid)); accepted = true; }
    if (promptStep == 1 && (value.length() == 0 || (value.length() >= 8 && value.length() <= 63))) { value.toCharArray(staged.password, sizeof(staged.password)); accepted = finished = true; }
  } else {
    value.trim();
    if (command == "set-api") {
      if (promptStep == 0) {
        if (!value.length()) value = "BMS";
        if (validHost(value)) { value.toCharArray(staged.apiHost, sizeof(staged.apiHost)); accepted = true; }
      } else {
        if (!value.length()) value = "2026";
        bool numeric = value.length() <= 5;
        for (uint16_t i = 0; i < value.length(); ++i) if (!isdigit(value[i])) numeric = false;
        long port = value.toInt();
        if (numeric && port > 0 && port <= 65535) { staged.apiPort = port; accepted = finished = true; }
      }
    } else if (command == "set-sensor") {
      if (validMac(value)) { value.toUpperCase(); value.toCharArray(staged.sensorMac, sizeof(staged.sensorMac)); accepted = finished = true; }
    } else if (command == "set-ip") {
      if (promptStep == 0) {
        accepted = parseIp(value, staged.ip, false);
        if (accepted) { Serial.print("IP address received: "); Serial.println(value); }
      } else if (promptStep == 1) {
        if (value.length() == 0) value = "255.255.255.0";
        accepted = parseIp(value, staged.subnet, true);
      } else if (promptStep == 2) accepted = parseIp(value, staged.gateway, false);
      else {
        if (!value.length()) value = IPAddress(staged.gateway).toString();
        accepted = parseIp(value, staged.dns, false);
        if (accepted && !validNetwork(staged)) { Serial.println("Invalid subnet, gateway, or host address; update cancelled."); command = ""; return; }
        if (accepted) { staged.ipConfigured = 1; finished = true; }
      }
    }
  }
  if (!accepted) { Serial.println("Invalid value. Try again or enter cancel."); printPrompt(); return; }
  if (!finished) { ++promptStep; printPrompt(); return; }
  // Merge only the prompted fields: an API MAC update during a serial prompt
  // must not be rolled back by saving Wi-Fi, IP, or API settings.
  Configuration updated = configuration;
  if (command == "set-wifi") {
    memcpy(updated.ssid, staged.ssid, sizeof(updated.ssid));
    memcpy(updated.password, staged.password, sizeof(updated.password));
  } else if (command == "set-ip") {
    memcpy(updated.ip, staged.ip, 4);
    memcpy(updated.subnet, staged.subnet, 4);
    memcpy(updated.gateway, staged.gateway, 4);
    memcpy(updated.dns, staged.dns, 4);
    updated.ipConfigured = staged.ipConfigured;
  } else if (command == "set-api") {
    memcpy(updated.apiHost, staged.apiHost, sizeof(updated.apiHost));
    updated.apiPort = staged.apiPort;
  } else if (command == "set-sensor") {
    memcpy(updated.sensorMac, staged.sensorMac, sizeof(updated.sensorMac));
  }
  bool sensorChanged = strcmp(updated.sensorMac, configuration.sensorMac) != 0;
  const bool needsRestart = command == "set-ip" || command == "set-wifi";
  if (sensorChanged) {
    sensorReadingReceived = false;
    const uint32_t now = millis(); noInterrupts(); control.sensorChanged(now); interrupts();
  }
  const bool changed = memcmp(&updated, &configuration, sizeof(Configuration)) != 0;
  configuration = updated;
  if (changed) persist();
  if (needsRestart) networkNeedsRestart = true;
  command = "";
  Serial.println(needsRestart ? "Configuration saved. Type reset to apply network changes." : "Configuration saved.");
}

void processSerial() {
  while (Serial.available()) {
    char character = Serial.read();
    if (character == '\r') continue;
    if (character == '\n') {
      if (lineOverflow) Serial.println("Input too long. Existing configuration preserved; enter a shorter value.");
      else handleLine(serialLine);
      serialLine = ""; lineOverflow = false;
    } else if (serialLine.length() < 127) serialLine += character;
    else lineOverflow = true;
  }
}

void setup() {
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, HIGH);
  startControlTimer();
  Serial.begin(115200);  // Never wait for a serial monitor on unattended boot.
  serialLine.reserve(128);
  configuration = configurationStorage.read();
  if (!storedValid(configuration)) configuration = defaults();
  readRadioFirmware(false);
  printRadioStatus();
  startBle();
  lastPoll = millis() - POLL_INTERVAL_MS;
  Serial.println("BMS controller ready. Heating ON at startup. Type help.");
}

void loop() {
  processSerial();
  if (bleReady) BLE.poll();
  else if (uint32_t(millis() - lastBleAttempt) >= 10000UL) startBle();
  const uint32_t now = millis();
  if (uint32_t(now - lastPoll) >= POLL_INTERVAL_MS) { lastPoll = now; pollApi(); }
}
