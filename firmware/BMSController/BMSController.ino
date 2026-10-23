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
#include "RecoveryLogic.h"
#include "SettingsValidation.h"
#include "ManagementHttp.h"
#include "SetupPage.h"
#include "BleActivityTrace.h"

#if !defined(ARDUINO_SAMD_MKRWIFI1010)
#error "Select Arduino MKR WiFi 1010 (arduino:samd:mkrwifi1010)."
#endif

const uint8_t HEATER_PIN = 4;
const uint32_t CONFIG_MAGIC = 0x424D5301;
const uint32_t POLL_INTERVAL_MS = 60000UL;
const uint32_t NINA_STARTUP_WAIT_MS = 1500UL;
const uint32_t SENSOR_LOG_INTERVAL_MS = 1000UL;
const uint32_t NETWORK_CHECK_MS = 1000UL;
const uint32_t NETWORK_RETRY_MS = 10000UL;
const uint32_t HTTP_TIMEOUT_MS = 3000UL;

struct CommandReceipt {
  uint32_t magic;
  char lastCommandId[37];
  uint8_t stationProvisioned;
  uint32_t checksum;
};

FlashStorage(configurationStorage, Configuration);
FlashStorage(receiptStorage, CommandReceipt);
Configuration configuration;
Configuration staged;
CommandReceipt receipt;
volatile ControlLogic control;
volatile RecoveryLogic recovery;
char deviceId[33];
char setupSsid[27];
WiFiServer managementServer(80);
WiFiClient managementClient;
ManagementHttp managementRequest;
bool managementActive = false;
bool managementListening = false;
uint32_t managementStarted = 0;
bool setupMode = false;
bool networkStarted = false;
uint8_t liveWifiStatus = WL_NO_MODULE;
IPAddress currentIp;
uint32_t lastNetworkCheck = 0;
uint32_t lastNetworkAttempt = 0;
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
bool apiStateReceived = false;
String serialLine;
String command;
uint8_t promptStep = 0;
bool lineOverflow = false;

class ActivityStream : public Stream {
 public:
  BleActivityTrace trace;
  bool output = false;
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t value) override {
    if (trace.feed(char(value))) {
      const uint32_t now = millis(); noInterrupts(); recovery.advertisement(now); interrupts();
    }
    if (output && Serial) Serial.write(value);
    return 1;
  }
};
ActivityStream activityStream;

void serviceManagement(bool dispatch);

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
  const uint8_t ip[] = {192, 168, 0, 230}, subnet[] = {255, 255, 255, 0}, router[] = {192, 168, 0, 1};
  memcpy(value.ip, ip, 4); memcpy(value.subnet, subnet, 4);
  memcpy(value.gateway, router, 4); memcpy(value.dns, router, 4);
  value.ipConfigured = 1;
  value.checksum = checksum(value);
  return value;
}

bool validMac(const String& mac) {
  return BmsSettings::validMac(mac.c_str());
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

uint32_t receiptChecksum(const CommandReceipt& value) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(CommandReceipt, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619UL;
  return hash;
}

void persistReceipt() {
  receipt.magic = CONFIG_MAGIC;
  receipt.checksum = receiptChecksum(receipt);
  receiptStorage.write(receipt);
}

void identifyDevice() {
  // SAMD21 factory unique identifier, independent of NINA or its Wi-Fi state.
  const uint32_t* words[] = {reinterpret_cast<const uint32_t*>(0x0080A00C), reinterpret_cast<const uint32_t*>(0x0080A040),
    reinterpret_cast<const uint32_t*>(0x0080A044), reinterpret_cast<const uint32_t*>(0x0080A048)};
  snprintf(deviceId, sizeof(deviceId), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(*words[0]),
    static_cast<unsigned long>(*words[1]), static_cast<unsigned long>(*words[2]), static_cast<unsigned long>(*words[3]));
  snprintf(setupSsid, sizeof(setupSsid), "BMS-Setup-%.8s", deviceId + 24);
}

// No SPI, heap allocation, or serial output is allowed in this interrupt.
void TC5_Handler() {
  if (TC5->COUNT16.INTFLAG.bit.MC0) {
    TC5->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
    const uint32_t now = millis();
    control.tick(now);
    digitalWrite(HEATER_PIN, control.heaterOn ? HIGH : LOW);
    if (recovery.due(now)) {
      digitalWrite(HEATER_PIN, HIGH);
      NVIC_SystemReset();
    }
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
  const uint32_t now = millis();
  noInterrupts(); recovery.advertisement(now); interrupts();
  String address = peripheral.address();
  address.toUpperCase();
  if (!strlen(configuration.sensorMac) || address != configuration.sensorMac || !peripheral.hasManufacturerData()) return;
  const int length = peripheral.manufacturerDataLength();
  if (length < 26 || length > 31) return;
  uint8_t manufacturer[31];
  peripheral.manufacturerData(manufacturer, length);
  double temperature;
  if (!decodeRuuvi(manufacturer, length, temperature)) return;
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
  if (setupMode) {
    Serial.print("Setup Wi-Fi (open): "); Serial.println(setupSsid);
    Serial.println("Setup page: http://192.168.0.230 (10-minute recovery suspended during setup)");
  }
  if (!strlen(configuration.ssid)) Serial.println("Use the setup Wi-Fi/page or set-wifi, then reset.");
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
  BLE.debug(activityStream);
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
  // Receive requests incrementally, but defer mutations until the API operation is complete.
  serviceManagement(false);
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
  apiStateReceived = true;
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
  if (!radioCompatible || networkNeedsRestart || setupMode || liveWifiStatus != WL_CONNECTED) return;
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

void stopManagement() {
  managementActive = false;
  managementClient.stop();
  if (managementListening) managementServer.end();
  managementListening = false;
}

void startManagement() {
  if (managementListening) return;
  managementServer.begin();
  managementListening = bool(managementServer);
  if (managementListening) Serial.println("Controller web management listening on port 80.");
}

void startSetupAp() {
  noInterrupts(); recovery.suspend(); interrupts();
  setupMode = true;
  networkStarted = true;
  stopManagement();
  WiFi.disconnect();
  const IPAddress setupIp(192, 168, 0, 230), mask(255, 255, 255, 0);
  WiFi.config(setupIp, setupIp, setupIp, mask);
  liveWifiStatus = WiFi.beginAP(setupSsid);
  lastNetworkAttempt = millis();
  if (liveWifiStatus == WL_AP_LISTENING || liveWifiStatus == WL_AP_CONNECTED) {
    currentIp = setupIp;
    startManagement();
    Serial.print("Join open setup Wi-Fi "); Serial.print(setupSsid);
    Serial.println(" and open http://192.168.0.230");
  } else Serial.println("Setup access point unavailable; retrying in 10 seconds.");
}

void connectStation(bool allowSetupFallback) {
  networkStarted = true;
  lastNetworkAttempt = millis();
  Serial.print("Connecting to Wi-Fi: "); Serial.println(configuration.ssid);
  WiFi.config(IPAddress(configuration.ip), IPAddress(configuration.dns), IPAddress(configuration.gateway), IPAddress(configuration.subnet));
  WiFi.setTimeout(3000);
  if (strlen(configuration.password)) WiFi.begin(configuration.ssid, configuration.password);
  else WiFi.begin(configuration.ssid);
  liveWifiStatus = WiFi.status();
  const uint32_t now = millis();
  noInterrupts(); recovery.wifi(liveWifiStatus == WL_CONNECTED, now); interrupts();
  if (liveWifiStatus == WL_CONNECTED) {
    currentIp = WiFi.localIP();
    if (!receipt.stationProvisioned) { receipt.stationProvisioned = 1; persistReceipt(); }
    startManagement();
    Serial.print("Wi-Fi connected. Current IP address: "); Serial.println(currentIp);
  } else {
    currentIp = IPAddress(0, 0, 0, 0);
    Serial.print("Wi-Fi connection failed: "); Serial.println(wifiStatusText(liveWifiStatus));
    if (allowSetupFallback && !receipt.stationProvisioned) startSetupAp();
  }
}

void serviceNetwork() {
  if (!radioCompatible) return;
  if (!networkStarted) {
    if (!strlen(configuration.ssid)) startSetupAp();
    else connectStation(true);
    return;
  }
  const uint32_t now = millis();
  if (uint32_t(now - lastNetworkCheck) < NETWORK_CHECK_MS) return;
  lastNetworkCheck = now;
  liveWifiStatus = WiFi.status();
  if (setupMode) {
    if (liveWifiStatus == WL_AP_LISTENING || liveWifiStatus == WL_AP_CONNECTED) startManagement();
    else if (uint32_t(now - lastNetworkAttempt) >= NETWORK_RETRY_MS) startSetupAp();
    return;
  }
  const bool connected = liveWifiStatus == WL_CONNECTED;
  noInterrupts(); recovery.wifi(connected, now); interrupts();
  if (connected) { currentIp = WiFi.localIP(); startManagement(); }
  else {
    currentIp = IPAddress(0, 0, 0, 0);
    if (managementListening) stopManagement();
    if (!networkNeedsRestart && uint32_t(now - lastNetworkAttempt) >= NETWORK_RETRY_MS) connectStation(false);
  }
}

void statusDocument(JsonDocument& document) {
  const uint32_t now = millis();
  noInterrupts();
  const double target = control.target, negative = control.negativeDelta, positive = control.positiveDelta;
  const double temperature = control.temperature;
  const bool heater = control.heaterOn, api = control.hasApiState, advertisement = recovery.advertisementSeen;
  const uint32_t sensorStamp = control.sensorStamp, apiStamp = control.apiStamp, bleStamp = recovery.advertisementStamp;
  const bool restarting = recovery.restartPending;
  interrupts();
  document["device_id"] = deviceId;
  document["uptime_ms"] = now;
  document["wifi_connected"] = !setupMode && liveWifiStatus == WL_CONNECTED;
  document["wifi_status"] = liveWifiStatus;
  if (setupMode || liveWifiStatus == WL_CONNECTED) document["current_ip"] = currentIp.toString();
  else document["current_ip"] = nullptr;
  document["ble_scanning"] = bleReady;
  document["nina_firmware"] = ninaFirmware;
  document["heater_on"] = heater;
  document["target_temperature"] = target;
  document["negative_delta"] = negative;
  document["positive_delta"] = positive;
  document["settings_source"] = api ? "api" : "defaults";
  if (apiStateReceived) document["api_age_ms"] = uint32_t(now - apiStamp); else document["api_age_ms"] = nullptr;
  if (sensorReadingReceived) {
    document["sensor_temperature"] = temperature;
    document["sensor_age_ms"] = uint32_t(now - sensorStamp);
  } else { document["sensor_temperature"] = nullptr; document["sensor_age_ms"] = nullptr; }
  if (advertisement) document["ble_age_ms"] = uint32_t(now - bleStamp); else document["ble_age_ms"] = nullptr;
  document["setup_mode"] = setupMode;
  document["restart_pending"] = restarting;
  document["last_command_id"] = receipt.lastCommandId;
  JsonObject config = document["config"].to<JsonObject>();
  config["ssid"] = configuration.ssid;
  config["password_set"] = bool(strlen(configuration.password));
  config["ip"] = IPAddress(configuration.ip).toString();
  config["subnet"] = IPAddress(configuration.subnet).toString();
  config["gateway"] = IPAddress(configuration.gateway).toString();
  config["dns"] = IPAddress(configuration.dns).toString();
  config["api_host"] = configuration.apiHost;
  config["api_port"] = configuration.apiPort;
  config["sensor_mac"] = configuration.sensorMac;
}

void sendHttp(int status, const char* type, const char* body, size_t length) {
  char header[180];
  const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 409 ? "Conflict" :
    status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Bad Request";
  const int count = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
    status, reason, type, unsigned(length));
  managementClient.write(reinterpret_cast<const uint8_t*>(header), count);
  managementClient.write(reinterpret_cast<const uint8_t*>(body), length);
}

void sendJson(int status, const JsonDocument& document) {
  char payload[2048];
  if (measureJson(document) >= sizeof(payload)) {
    const char error[] = "{\"error\":{\"code\":\"response_too_large\",\"message\":\"Controller response exceeds its size limit.\"}}";
    sendHttp(400, "application/json", error, sizeof(error) - 1);
    return;
  }
  const size_t length = serializeJson(document, payload, sizeof(payload));
  sendHttp(status, "application/json", payload, length);
}

void managementError(int status, const char* code, const char* message) {
  JsonDocument document;
  document["error"]["code"] = code;
  document["error"]["message"] = message;
  sendJson(status, document);
}

bool copyJsonText(JsonVariantConst field, char* output, size_t capacity) {
  if (!field.is<const char*>()) return false;
  const JsonString text = field.as<JsonString>();
  if (text.size() >= capacity || strlen(text.c_str()) != text.size()) return false;
  memcpy(output, text.c_str(), text.size() + 1);
  return true;
}

bool applyConfigPatch(JsonObjectConst patch, Configuration& candidate) {
  if (!patch.size()) return false;
  for (JsonPairConst entry : patch) {
    const char* key = entry.key().c_str();
    JsonVariantConst value = entry.value();
    bool valid = false;
    if (!strcmp(key, "ssid")) valid = copyJsonText(value, candidate.ssid, sizeof(candidate.ssid));
    else if (!strcmp(key, "password")) valid = copyJsonText(value, candidate.password, sizeof(candidate.password));
    else if (!strcmp(key, "api_host")) valid = copyJsonText(value, candidate.apiHost, sizeof(candidate.apiHost));
    else if (!strcmp(key, "sensor_mac")) valid = copyJsonText(value, candidate.sensorMac, sizeof(candidate.sensorMac));
    else if (!strcmp(key, "api_port")) {
      valid = value.is<unsigned int>() && value.as<unsigned int>() > 0 && value.as<unsigned int>() <= 65535;
      if (valid) candidate.apiPort = value.as<unsigned int>();
    } else {
      uint8_t* address = !strcmp(key, "ip") ? candidate.ip : !strcmp(key, "subnet") ? candidate.subnet :
        !strcmp(key, "gateway") ? candidate.gateway : !strcmp(key, "dns") ? candidate.dns : nullptr;
      char text[16];
      valid = address && copyJsonText(value, text, sizeof(text)) && BmsSettings::parseIpv4(text, address);
    }
    if (!valid) return false;
  }
  for (char* byte = candidate.sensorMac; *byte; ++byte) *byte = toupper(static_cast<unsigned char>(*byte));
  candidate.ipConfigured = 1;
  return BmsSettings::valid(candidate, true);
}

void acceptedCommand(const char* id, bool newCommand) {
  if (newCommand) {
    // Bound response/close stalls too. A completed response shortens this to 500 ms.
    const uint32_t now = millis(); noInterrupts(); recovery.schedule(now, 5000); interrupts();
  }
  JsonDocument response;
  response["command_id"] = id; response["accepted"] = true; response["restart_required"] = true;
  sendJson(202, response);
  if (newCommand) { const uint32_t now = millis(); noInterrupts(); recovery.schedule(now, 500); interrupts(); }
}

void dispatchManagement() {
  const char* method = managementRequest.method;
  const char* path = managementRequest.path;
  if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/setup"))) {
    sendHttp(200, "text/html; charset=utf-8", SETUP_PAGE, sizeof(SETUP_PAGE) - 1);
    return;
  }
  if (!strcmp(method, "GET") && !strcmp(path, "/status")) {
    JsonDocument document; statusDocument(document); sendJson(200, document); return;
  }
  const bool configCommand = !strcmp(path, "/config"), restartCommand = !strcmp(path, "/restart");
  if (!configCommand && !restartCommand) { managementError(404, "not_found", "Controller endpoint was not found."); return; }
  if (strcmp(method, "POST")) { managementError(405, "method_not_allowed", "Use POST for this operation."); return; }
  JsonDocument body;
  if (deserializeJson(body, managementRequest.body(), DeserializationOption::NestingLimit(4)) || !body.is<JsonObject>()) {
    managementError(400, "invalid_request", "Send a valid JSON command object."); return;
  }
  for (JsonPair entry : body.as<JsonObject>()) {
    const char* key = entry.key().c_str();
    if (strcmp(key, "command_id") && strcmp(key, "device_id") && (!configCommand || strcmp(key, "config"))) {
      managementError(400, "invalid_request", "Unknown command field."); return;
    }
  }
  char id[37], expectedDevice[33];
  if (!copyJsonText(body["command_id"], id, sizeof(id)) || !BmsSettings::validCommandId(id) ||
      !copyJsonText(body["device_id"], expectedDevice, sizeof(expectedDevice)) ||
      (configCommand && !body["config"].is<JsonObject>())) {
    managementError(400, "invalid_request", "A UUID command_id, device_id and valid config object are required."); return;
  }
  for (char* byte = expectedDevice; *byte; ++byte) *byte = tolower(static_cast<unsigned char>(*byte));
  if (strcmp(expectedDevice, deviceId)) { managementError(409, "device_mismatch", "A different controller is using this address."); return; }
  if (!strcmp(id, receipt.lastCommandId)) { acceptedCommand(id, false); return; }
  noInterrupts(); const bool pending = recovery.restartPending; interrupts();
  if (pending) { managementError(409, "restart_pending", "The controller is already restarting."); return; }
  Configuration candidate = configuration;
  if (configCommand && !applyConfigPatch(body["config"].as<JsonObjectConst>(), candidate)) {
    managementError(400, "invalid_configuration", "Invalid controller settings; existing settings were preserved."); return;
  }
  if (configCommand) {
    const bool wifiChanged = strcmp(candidate.ssid, configuration.ssid) || strcmp(candidate.password, configuration.password);
    const bool sensorChanged = strcmp(candidate.sensorMac, configuration.sensorMac);
    if (wifiChanged) receipt.stationProvisioned = 0;
    configuration = candidate;
    persist();
    if (sensorChanged) {
      sensorReadingReceived = false;
      const uint32_t now = millis(); noInterrupts(); control.sensorChanged(now); interrupts();
    }
    networkNeedsRestart = true;
    command = "";  // Abandon a partial serial prompt after an accepted web update.
  }
  strcpy(receipt.lastCommandId, id);
  persistReceipt();
  acceptedCommand(id, true);
}

void serviceManagement(bool dispatch) {
  if (!managementListening) return;
  if (!managementActive) {
    managementClient = managementServer.available();
    if (!managementClient) return;
    managementActive = true;
    managementStarted = millis();
    managementRequest.reset();
  }
  if (!managementRequest.complete && !managementRequest.error) {
    uint16_t budget = 128;
    while (budget-- && managementClient.available() && !managementRequest.complete && !managementRequest.error) {
      const int value = managementClient.read();
      if (value < 0) break;
      managementRequest.feed(char(value));
    }
    if (!managementRequest.complete && uint32_t(millis() - managementStarted) >= HTTP_TIMEOUT_MS) managementRequest.error = 400;
  }
  if (!dispatch) return;
  if (managementRequest.error) managementError(400, "invalid_request", "HTTP request is invalid, too large or incomplete.");
  else if (managementRequest.complete) dispatchManagement();
  else if (managementClient.connected()) return;
  managementActive = false;
  managementClient.stop();
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
  return BmsSettings::validHost(value.c_str());
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
  return BmsSettings::validNetwork(value);
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
      activityStream.output = true;
      BLE.debug(activityStream);
      Serial.println("BLE command tracing enabled until restart. No configuration was erased.");
      if (!bleReady) { readRadioFirmware(false); startBle(); }
      printRadioStatus();
      return;
    }
    if (value == "reset") { Serial.println("Restarting; stored configuration preserved."); delay(25); NVIC_SystemReset(); }
    if (value == "factory-reset") {
      digitalWrite(HEATER_PIN, HIGH);
      configuration = defaults(); persist();
      memset(&receipt, 0, sizeof(receipt)); persistReceipt();
      Serial.println("Factory configuration restored. Restarting."); delay(25); NVIC_SystemReset();
    }
    if (value != "set-wifi" && value != "set-ip" && value != "set-api" && value != "set-sensor") { Serial.println("Unknown command. Type help."); return; }
    command = value; staged = configuration; promptStep = 0; printPrompt(); return;
  }
  if (value == "cancel") { command = ""; Serial.println("Update cancelled; existing configuration preserved."); return; }
  bool finished = false, accepted = false;
  if (command == "set-wifi") {
    if (promptStep == 0 && value.length() > 0 && value.length() < sizeof(staged.ssid)) { value.toCharArray(staged.ssid, sizeof(staged.ssid)); accepted = true; }
    if (promptStep == 1 && BmsSettings::validPassword(value.c_str())) { value.toCharArray(staged.password, sizeof(staged.password)); accepted = finished = true; }
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
  const bool wifiChanged = strcmp(updated.ssid, configuration.ssid) || strcmp(updated.password, configuration.password);
  configuration = updated;
  if (changed) persist();
  if (wifiChanged) { receipt.stationProvisioned = 0; persistReceipt(); }
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
  identifyDevice();
  configuration = configurationStorage.read();
  if (!storedValid(configuration)) configuration = defaults();
  // Older installations without network settings receive defaults; saved static IPs are retained.
  if (!configuration.ipConfigured) {
    const Configuration baseline = defaults();
    memcpy(configuration.ip, baseline.ip, 4); memcpy(configuration.subnet, baseline.subnet, 4);
    memcpy(configuration.gateway, baseline.gateway, 4); memcpy(configuration.dns, baseline.dns, 4);
    configuration.ipConfigured = 1;
    persist();
  }
  receipt = receiptStorage.read();
  if (receipt.magic != CONFIG_MAGIC || receipt.checksum != receiptChecksum(receipt) || receipt.stationProvisioned > 1 ||
      !memchr(receipt.lastCommandId, 0, sizeof(receipt.lastCommandId)) ||
      (receipt.lastCommandId[0] && !BmsSettings::validCommandId(receipt.lastCommandId))) memset(&receipt, 0, sizeof(receipt));
  setupMode = !strlen(configuration.ssid);
  if (!setupMode) { const uint32_t now = millis(); noInterrupts(); recovery.arm(now); interrupts(); }
  readRadioFirmware(false);
  printRadioStatus();
  startBle();
  serviceNetwork();
  lastPoll = millis() - POLL_INTERVAL_MS;
  Serial.println("BMS controller ready. Heating ON at startup. Type help.");
}

void loop() {
  processSerial();
  if (bleReady) BLE.poll();
  else if (uint32_t(millis() - lastBleAttempt) >= 10000UL) startBle();
  serviceNetwork();
  serviceManagement(true);
  const uint32_t now = millis();
  noInterrupts(); const bool pending = recovery.restartPending; interrupts();
  if (!managementActive && !pending && uint32_t(now - lastPoll) >= POLL_INTERVAL_MS) { lastPoll = now; pollApi(); }
}
