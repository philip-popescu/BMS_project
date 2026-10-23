# BMS Project Specifications

**Source:** `description.txt`, `new_features.md`, the user's follow-up clarifications, and the decisions entered in section 10 of this file.

**Status:** Updated on 2026-10-23 with the confirmed controller-management and web-provisioning requirements.

**Units:** Temperatures and temperature differences are expressed in degrees Celsius.

Confirmed requirements come from the description, `new_features.md`, its clarification section, the user's follow-up answers, and their edits to section 10. JSON field names, the frontend port, and the additional API routes selected here are implementation decisions. Section 10 records the resolved decisions; section 11 defines the UTC scheduling convention implementing the confirmed daylight-saving policy. The reset commands defined here supersede the original description's use of `reset` to clear configuration.

## 1. Purpose and scope

Build a home heating management system consisting of a Raspberry Pi 4 Model B, a React web interface, and a fleet of Arduino MKR WiFi 1010 controllers. The system must support multiple households, each containing multiple rooms. Each room has one assigned controller and one assigned RuuviTag temperature sensor.

Users configure households, rooms, device assignments, and temperature programs through the web interface. The Raspberry Pi determines each room's current temperature target. Controllers retrieve their state from the Raspberry Pi and independently operate the heater connected to their room.

Required deliverables:

1. Arduino controller firmware, including persistent serial and web configuration, first-time Wi-Fi setup through a web page, status reporting, remote restart, and automatic radio recovery.
2. A Python Flask web API hosted on the Raspberry Pi and listening on port `2026`, with configuration stored in JSON files.
3. A React frontend that communicates with the API and is served on port `3000`.
4. A Raspberry Pi installation script that installs the required frameworks and applications.
5. Two `.desktop` files that launch the API and frontend servers on login.

All households share the same local network. The API and frontend must be accessible only on that network, and authentication is not required. Remote cloud access, user accounts, energy reporting, and temperature history are outside the requested scope.

## 2. System architecture

| Component | Responsibilities |
| --- | --- |
| Raspberry Pi API | Run on port `2026`, persist configuration, evaluate UTC programs, identify controllers by fixed IP, and proxy controller status, settings, and restart requests over the local network. |
| React frontend | Run on port `3000`; manage households, rooms/zones, programs, deltas and assignments; provide controller status, settings, restart, and commissioning panels. |
| Arduino controller | Read RuuviTag advertisements, poll the API once per minute, control heating, serve a local management/setup page and JSON interface, and restart after sustained Wi-Fi or Bluetooth reception loss. |
| RuuviTag | Supply temperature measurements for its assigned room. |

Controllers reach the Raspberry Pi API through Wi-Fi on the shared local network. Their fixed IP addresses can be configured through the web interface or the retained serial commands. A room's configured controller IP must match the address seen by the API and remain unique across households. A room and a zone refer to the same entity.

First-time Wi-Fi setup must work entirely through a controller-hosted web page. A new controller provides a temporary setup Wi-Fi network and serves that page at `http://192.168.0.230`. Its default station IP is also `192.168.0.230`. Commission one default-address controller at a time, assigning each controller a unique operating address before introducing another. Section 4.5 defines this workflow.

The Raspberry Pi hostname is `BMS`. The default API host and port are `BMS:2026`. Hostname resolution must be verified from the controllers and the computers using the frontend; configuring the API by IP address must also be supported.

The deployment must target the latest stable Raspberry Pi OS available when installation is prepared. Use a desktop installation to support the requested login autostart files, and record the exact OS release and dependency versions tested. Port `3000` is selected for the frontend; port `2026` remains the required API port.

## 3. Domain model

### 3.1 Household

A household contains:

- A unique household identifier used by `/set_config` and `/get_config`.
- A collection of rooms.

**Selected configuration fields:** a display name and a household timezone. The frontend uses that timezone to display and enter local program times, while the API and JSON storage use UTC for schedule recurrence. Unless explicitly configured, the timezone defaults to the Raspberry Pi's configured timezone. The `Europe/Bucharest` value in the API example is illustrative.

### 3.2 Room

A room contains:

- One assigned controller, identified by its fixed IP address.
- One assigned RuuviTag, identified by its MAC address.
- A collection of temperature programs.
- Room-level `negative_delta` and `positive_delta` settings, each defaulting to `0.1 °C`.

**Selected configuration fields:** a stable room identifier and a display name. Deltas belong to rooms, as confirmed in the clarification section.

### 3.3 Temperature program

Each program defines a start time and a target temperature. A room can have multiple programs starting at different times. Programs repeat every day by default and may instead apply only to selected days of the week. Recurrence preserves UTC timing across daylight-saving changes, so displayed local start times and, near midnight, local weekdays may shift. The active temperature target carries across midnight until another applicable program starts.

**Selected configuration fields:** a stable program identifier, `start_time` expressed as UTC `HH:MM`, and an optional `days` array of UTC weekdays using full lowercase names, from `monday` through `sunday`. Omitting `days` means every UTC day. An explicit array must contain at least one day and no duplicates. The frontend converts local time and weekday selections into this convention as described in section 11.

### 3.4 Stored controller configuration

Each controller must persist the following settings across power loss:

- Wi-Fi SSID.
- Wi-Fi password.
- Manually assigned controller IP address.
- Subnet mask, gateway, and DNS settings needed for the selected network configuration.
- API host, supplied as an IP address or hostname/alias, defaulting to `BMS`.
- API port, with `2026` as the expected server port.
- MAC address of its assigned RuuviTag, set through serial/web configuration or updated by the API.

The sensor MAC is the only value from `/get_state` that persists across controller reboots. When that API supplies a different sensor MAC, it replaces the saved assignment. Explicit management changes through the web settings panel persist Wi-Fi, network, API, and sensor settings just like serial configuration. Target temperature and both deltas remain runtime-only and return to `23.0`, `0.1`, and `0.1 °C` after reboot until the next valid temperature-state response. The five-hour API fallback preserves the saved sensor MAC and network settings.

Controllers also persist the last management command identifier so a retried settings or restart request cannot repeatedly reboot the device. Management metadata does not change the runtime-only status of target temperature and deltas.

## 4. Arduino firmware requirements

### 4.1 Temperature acquisition

- Read temperature from the assigned RuuviTag using Bluetooth.
- Match sensor readings to the configured sensor MAC address.
- Use and persist the sensor MAC supplied by a successful `/get_state` response, replacing the previous saved assignment. Write persistent storage only when the MAC changes.
- Use the working `blePeripheralDiscoveredHandler` supplied in `description.txt` as the reference for decoding the user's sensors.

The supplied decoder recognizes manufacturer bytes `153` and `4`, accepts data format `5`, and converts a signed 16-bit temperature to Celsius using `raw_temperature * 0.005`. In its sample advertisement layout, the format is at byte `7` and the temperature is at bytes `8` and `9`, with the most significant temperature byte first.

The implementation must check data lengths before accessing bytes and reject malformed or unsupported advertisements. The provided function demonstrates decoding for the user's sensors; firmware commissioning must also verify that polling over Wi-Fi and reading Bluetooth advertisements work together on the actual controller. Record the working firmware and library versions rather than treating the user's statement of "latest firmware" as a fixed version number.

Maintain a timestamp for the most recent valid temperature reading from the assigned sensor. A malformed advertisement, an unsupported format, or a reading from another sensor must not refresh that timestamp. Scan often enough to obtain valid readings within the confirmed 30-second timeout during normal operation.

If more than `30 seconds` pass without a valid reading, heating must automatically switch ON. After a sensor assignment changes, discard the previous sensor's cached reading and start a new 30-second acquisition timer. **Selected acquisition behavior:** retain the existing heater state while waiting for the first new reading, then force ON if the timer exceeds 30 seconds. On startup, heating is already ON while waiting for the first reading.

### 4.2 Heater control

The heater control output must use board pin `4`, with `HIGH` meaning heating ON and `LOW` meaning heating OFF. **On every startup or reboot, set pin `4` HIGH so heating starts ON**, before waiting for Wi-Fi, the API, or a sensor reading. Normal temperature control starts when a valid reading for the assigned sensor is available.

Default runtime settings are:

| Setting | Default |
| --- | --- |
| Target temperature | `23.0 °C` |
| Negative delta | `0.1 °C` |
| Positive delta | `0.1 °C` |

Let `T` be the measured temperature, `S` the target temperature, `Dn` the negative delta, and `Dp` the positive delta.

- Switch the heater **on** when `T < S - Dn`.
- Switch the heater **off** when `T > S + Dp`.
- Retain the previous output state between these thresholds, including exact equality at either threshold. This selected hysteresis behavior follows the strict comparisons in the description.

For a target of `23.0 °C` and deltas of `0.1 °C`, this means switching on below `22.9 °C`, switching off above `23.1 °C`, and retaining the previous state within that interval.

The clarified description confirms `target + positive_delta` as the heater-off threshold. The follow-up answers confirm active-HIGH output and switching heating ON after more than 30 seconds without a valid sensor reading.

The missing-sensor rule takes priority over temperature hysteresis: set pin `4` HIGH when the reading age exceeds 30 seconds, even if the heater was previously OFF. At exactly 30 seconds, the reading has not yet exceeded the timeout. Once a fresh valid reading arrives, immediately resume normal temperature control. Use an elapsed-time clock for the timeout so Wi-Fi polling and serial interactions do not delay enforcing it.

### 4.3 API synchronization

- Send `GET /get_state` to the configured API host and port once per minute.
- Allow the API to identify the requesting controller by its fixed source IP address.
- Apply the returned target temperature, negative delta, positive delta, and sensor MAC address.
- Use the documented default target and deltas after every reboot and before the first successful API response; load the sensor MAC from persistent storage.
- Persist only the API-provided sensor MAC. Do not persist the API-provided target temperature or deltas on the controller.

Validate the entire response before applying it. A request timeout, connection failure, non-success response, or invalid payload must not partially update the active state or refresh the last-success timestamp.

**Confirmed outage behavior:**

1. Measure elapsed time since the last fully valid `/get_state` response using an elapsed-time clock.
2. For less than five hours without a valid response, retain the last valid runtime target and deltas and continue temperature control using the assigned sensor.
3. At five hours (`18,000 seconds`) without a valid response, switch the target to `23.0 °C` and both deltas to `0.1 °C`. Preserve the sensor MAC, Wi-Fi credentials, static network settings, and API host/port.
4. Continue polling once per minute after fallback. Apply the next valid API state immediately and start a new five-hour interval from that response.
5. Before the first valid API response after a reboot, use the documented temperature defaults; no previous runtime target or deltas are restored.

The five-hour fallback changes temperature settings rather than directly forcing the heater output. The independent 30-second missing-sensor rule still forces heating ON whenever valid sensor readings are absent. Neither timeout may be delayed by a blocking network request or serial interaction.

### 4.4 Serial configuration

Use the Arduino IDE serial monitor. Commands must be single tokens, followed by prompts for required fields. The firmware must accept newline or CRLF termination and report whether each update succeeded.

The selected command vocabulary is:

| Command | Prompts or result |
| --- | --- |
| `help` | List supported commands and their purposes. |
| `set-wifi` | Prompt for SSID and password, then save the Wi-Fi settings. |
| `set-ip` | Prompt first for the fixed IP address, then subnet mask, gateway, and DNS as needed; save the static network settings. |
| `set-api` | Prompt for API host and port; use `BMS` and `2026` as defaults. |
| `set-sensor` | Prompt for the RuuviTag MAC address, then save the sensor assignment. |
| `reset` | Restart the hardware while preserving all stored configuration. |
| `factory-reset` | Clear the controller's stored configuration, restore defaults, and restart the hardware. |

Example interaction:

```text
> set-ip
Enter IP address:
> 192.168.0.15
Fixed IP set to 192.168.0.15
```

The example shows the IP-address portion of the interaction. Collect any remaining network fields through subsequent prompts, and confirm that the complete configuration has been saved after validation. Reject invalid input without overwriting the previous complete network configuration.

Serial-configured settings must survive power loss. **Selected baud rate:** `115200`. Applying network settings must be documented, including whether the firmware reconnects immediately or requires a restart.

**Confirmed restart behavior:** `reset` restarts the controller without clearing Wi-Fi credentials, static network settings, API host/port, or the saved sensor MAC, including a MAC previously updated by the API. As on every reboot, heating starts ON and the runtime target and deltas return to their defaults until the next valid API response.

**Factory reset behavior:** `factory-reset` clears saved Wi-Fi credentials, custom network settings, the sensor MAC, and management command metadata; restores the default address `192.168.0.230`, API host `BMS`, port `2026`, and runtime temperature defaults; then restarts into web setup. Heating starts ON and remains ON while no assigned sensor supplies a valid reading. The controller can be configured again entirely through its setup web page; serial configuration remains available.

Both commands affect only the controller receiving the serial command. They do not delete household, room, or program JSON files on the Raspberry Pi.

### 4.5 First-time web setup and default network

- A new or factory-reset controller must start a temporary open Wi-Fi access point named `BMS-Setup-<device suffix>` when it has no saved Wi-Fi SSID. The suffix distinguishes nearby controllers.
- Connect a phone or computer to that network and open `http://192.168.0.230`. Internet access and a running Raspberry Pi connection are not required to load the embedded page. A captive-portal redirect is not required.
- The page must configure Wi-Fi SSID/password, controller IP, subnet, gateway, DNS, API host/port, and an optional initial sensor MAC. It must work without serial interaction or externally hosted scripts, fonts, or other assets.
- Defaults are controller IP `192.168.0.230`, subnet `255.255.255.0`, gateway/DNS `192.168.0.1`, and API host/port `BMS:2026`. The setup access point itself remains reachable at `.230` while settings are edited.
- Validate the complete resulting configuration before saving. An omitted Wi-Fi password preserves the existing one; explicitly choosing an open network saves an empty password. Never return a saved password in status or configuration responses.
- Report that changes were accepted before restarting with the new saved settings. The browser may need to rejoin the household Wi-Fi and open the controller's newly configured address afterward.
- Provide a setup recovery path when saved Wi-Fi credentials cannot establish a connection during initial startup, so an incorrectly entered SSID/password can be corrected through the web page.
- Only one default-address controller may be connected to the household LAN at a time. Configure a unique operating IP, then commission the next device. Existing valid saved IPs must be preserved; this default does not renumber deployed controllers.
- Setup mode is an explicit commissioning state. Automatic connectivity/reception restarts are suspended during setup to avoid preventing configuration. The independent heater startup and 30-second sensor rules continue.

### 4.6 Controller status, remote settings, and restart

Each controller must serve a bounded HTTP management interface on port `80`, in setup mode and while connected to the LAN. The main web application accesses LAN controllers through the Raspberry Pi API; the embedded setup page accesses its own controller directly.

`GET /status` returns a stable `device_id`, uptime, current Wi-Fi connection/status/IP, BLE scanning state, NINA firmware version, heater state, active target/deltas and source, assigned sensor temperature/reading age, age of any last BLE advertisement, last valid API response age, setup state, restart-pending state, last command identifier, and saved nonsecret configuration. Unavailable measurements/ages are `null`, never a fabricated temperature or a fresh timestamp. Expose only `password_set` for the saved password.

`POST /config` receives `{ "command_id": "...", "device_id": "...", "config": { ... } }`. Supported patch fields are `ssid`, `password`, `ip`, `subnet`, `gateway`, `dns`, `api_host`, `api_port`, and `sensor_mac`. Validate the complete resulting configuration, including subnet/gateway consistency, before one persistent save. Reject unknown fields, incorrect types, an unexpected device identity, and invalid or oversized requests. Omitted fields retain their saved values. A blank sensor assignment is permitted for initial provisioning; a room assigned on the Raspberry Pi must still have a valid sensor MAC.

`POST /restart` receives a command identifier and expected device identity. It preserves saved settings and restarts the hardware. Both settings and restart operations return `202 Accepted` with `command_id`, `accepted: true`, and `restart_required: true` before a short deferred reset. Persist the command identifier, and treat duplicate delivery as the same operation without another flash write or reboot. `restart_pending` becomes false on the next boot. Heating starts ON on every restart, and target/deltas return to defaults pending API synchronization.

Requests have bounded headers/body sizes and an overall receive deadline; incomplete or malformed requests must release the client. Management handling must allow BLE and heater processing to continue. SPI, serial output, JSON parsing, and flash writes must not run inside the heater interrupt.

### 4.7 Automatic connectivity and Bluetooth recovery

The user confirmed a **ten-minute** interval for both recovery conditions and that Bluetooth recovery counts **any received BLE advertisement**:

1. During normal station operation, restart the controller after Wi-Fi has remained disconnected for ten continuous minutes. Reconnection resets that outage interval.
2. Restart after ten minutes without any received BLE advertisement. Every advertisement refreshes this recovery timer, including unrelated devices and unsupported/malformed sensor payloads received by the BLE discovery handler.
3. Bluetooth recovery uses a separate timestamp from assigned-sensor temperature acquisition. Unrelated advertisements must never refresh the 30-second assigned-sensor timer or influence heater measurements.
4. Arm recovery for normal configured operation, while exempting the explicit web setup state in section 4.5. Begin timing from activation if no BLE advertisement has yet been received.
5. Use elapsed-time arithmetic that handles timer rollover. Recovery must still occur if a radio/library call blocks the main loop; a main-loop-only restart check is insufficient. Keep the independent heater control interrupt active.
6. Automatic restart preserves all saved settings, starts heating ON, and resets runtime target/deltas to defaults. An API outage alone, with working Wi-Fi and BLE reception, continues to use the five-hour fallback without requiring a restart.

## 5. Scheduling requirements

The Raspberry Pi must use each room's programs to determine the current target returned by `/get_state`.

Confirmed scheduling requirements:

1. Programs repeat every day by default.
2. Programs must also support selected days of the week.
3. The active temperature target carries across midnight until another applicable program starts.
4. Programs preserve their UTC timing across daylight-saving transitions; their displayed local times shift with the household timezone's UTC offset.

Selected scheduling rules:

1. Evaluate stored start times and applicable weekdays in UTC. Use the household timezone for local presentation and conversion of user-entered schedules.
2. A program becomes active at its start time and remains active until the next applicable start event. No explicit end time is required.
3. Choose the most recent applicable start event at or before the current time, including events from preceding days and the preceding week when necessary. This also determines the correct target after an API restart.
4. A room without programs uses the `23.0 °C` default target.
5. Program changes affect the next successful controller poll.

Example: a room with stored UTC programs `07:00 → 22.0 °C` and `21:00 → 19.0 °C` uses `22.0 °C` from 07:00 until 21:00 UTC and `19.0 °C` for the rest of the daily cycle. The frontend displays these boundaries in the household's local timezone.

For a stored program with `days: ["saturday", "sunday"]`, start events occur only on those UTC weekdays. Its target may carry into a following day until the next applicable program starts; neither local nor UTC midnight resets the target.

**Confirmed priority rule:** a day-specific program takes priority over an everyday program starting at the same time on that day. Apply this rule to the canonical UTC start times and weekdays.

**Selected collision validation:** reject two competing day-specific programs with overlapping days and the same start time, and reject duplicate everyday start times. A `days` array containing all seven weekdays is treated as an everyday program for this validation. Day-specific programs otherwise add start events to the daily schedule rather than replacing the whole day's schedule.

**Confirmed daylight-saving policy:** preserve UTC timing. Do not rewrite stored UTC start times or weekdays when the household timezone's offset changes. Recalculate the local display from the same UTC occurrences. For example, a daily event at `05:00 UTC` displays as `07:00` at UTC+02:00 and `08:00` at UTC+03:00; it returns to `07:00` when the offset changes back.

Each scheduled UTC occurrence is unique. A skipped local clock interval does not skip its UTC event, and a repeated local interval does not create a duplicate event. Select the active target by comparing UTC instants, including during the repeated clock interval. Section 11 defines conversion and persistence details.

Manual temperature overrides are outside the requested scope.

## 6. API requirements

The Flask API must listen on port `2026` and provide the routes below. Query parameter `id` refers to the household identifier; the quotation marks in the source examples are treated as explanatory notation, rather than part of the identifier. No endpoint requires authentication; access is limited to the shared local network.

The four original routes and their purposes come from the description. Human-readable JSON field names, payload structures, and response status codes are selected here. The user's section 10 decision authorizes additional routes; section 6.5 defines operations for individual rooms and programs. Keep `/set_config` as a complete configuration replacement operation and use the additional routes for ordinary frontend edits.

### 6.1 `GET /get_state`

Identify the controller by the request's source IP address, locate its assigned room, evaluate that room's current target, and return:

```json
{
  "target_temperature": 23.0,
  "negative_delta": 0.1,
  "positive_delta": 0.1,
  "sensor_mac": "AA:BB:CC:DD:EE:FF"
}
```

- Success: `200 OK`.
- Unknown controller IP: `404 Not Found` with a structured error.
- Controller IP assignments must be unambiguous across the configurations served by the same API instance.
- Controllers connect directly to the API over the shared local network; identify them from the connection's source IP rather than an arbitrary client-supplied IP header.

### 6.2 `PUT /set_config?id={household_id}`

Set the configuration of the identified household. If the identifier does not exist, create the household.

**Selected update semantics:** the request body replaces the household's complete configuration, including its room and program collections. Omitted rooms or programs are removed. This is a complete-snapshot operation, not a partial update. The `rooms` field is required, and each room must include its `programs` collection; an empty collection is valid. Validate the full request before saving any changes. Use the routes in section 6.5 when changing only one room or program.

Example request body; all program `start_time` and `days` values below are UTC. The household `timezone` controls local display, not the recurrence basis:

```json
{
  "name": "Main House",
  "timezone": "Europe/Bucharest",
  "rooms": [
    {
      "id": "living-room",
      "name": "Living Room",
      "controller_ip": "192.168.1.50",
      "sensor_mac": "AA:BB:CC:DD:EE:FF",
      "negative_delta": 0.1,
      "positive_delta": 0.1,
      "programs": [
        {
          "id": "morning",
          "start_time": "07:00",
          "target_temperature": 22.0
        },
        {
          "id": "evening",
          "start_time": "21:00",
          "target_temperature": 19.0
        },
        {
          "id": "weekend-daytime",
          "days": ["saturday", "sunday"],
          "start_time": "09:00",
          "target_temperature": 23.0
        }
      ]
    }
  ]
}
```

Responses:

- New household: `201 Created`.
- Updated household: `200 OK`.
- Response body: the saved configuration with its household `id` included.
- Missing identifier, malformed JSON, or invalid field values: `400 Bad Request`.
- Controller IP already assigned to another room: `409 Conflict`.

### 6.3 `GET /get_config?id={household_id}`

Return the stored configuration of the requested household, using the configuration structure above with its household `id` included.

- Success: `200 OK`.
- Missing identifier: `400 Bad Request`.
- Unknown household: `404 Not Found`.

### 6.4 `GET /get_house_list`

Return the list of existing households.

Response:

```json
[
  {
    "id": "main-house",
    "name": "Main House"
  }
]
```

Return `200 OK` with an empty array when no households exist.

### 6.5 Additional room and program routes

These routes are selected under the user's authorization to improve coordination between the frontend and backend. They operate within an existing household and preserve unrelated rooms and programs.

| Method and route | Operation |
| --- | --- |
| `POST /households/{household_id}/rooms` | Add a room using the room structure in section 6.2. Require a unique room `id`; omitted `programs` defaults to an empty array and omitted deltas default to `0.1 °C`. |
| `PATCH /households/{household_id}/rooms/{room_id}` | Update supplied room fields: `name`, `controller_ip`, `sensor_mac`, `negative_delta`, or `positive_delta`. Preserve the room's programs. |
| `DELETE /households/{household_id}/rooms/{room_id}` | Remove the room and its programs from the household. |
| `POST /households/{household_id}/rooms/{room_id}/programs` | Add a program using the program structure in section 6.2. Require a unique program `id`, `start_time`, and `target_temperature`; omitted `days` means every day. |
| `PATCH /households/{household_id}/rooms/{room_id}/programs/{program_id}` | Update supplied program fields: `start_time`, `target_temperature`, or `days`. Preserve omitted fields. |
| `DELETE /households/{household_id}/rooms/{room_id}/programs/{program_id}` | Remove the identified program while preserving other programs. |

Path identifiers refer to the same stored household, room, and program IDs used in the configuration. Identifiers are immutable through `PATCH`. Each operation must load, validate, and save the affected household atomically, including checking controller IP uniqueness across all households.

For `PATCH`, omitted fields remain unchanged; explicit `null`, unknown fields, and an empty update body are invalid. To change a day-specific program back to every day, supply all seven weekday names in `days`. Room patches must not contain a `programs` collection; use the program routes for those edits.

Example room update:

```json
{
  "negative_delta": 0.2,
  "positive_delta": 0.3
}
```

Example program update:

```json
{
  "days": ["monday", "tuesday", "wednesday", "thursday", "friday"],
  "start_time": "06:30"
}
```

Responses:

- Creation: `201 Created` with the saved room or program object.
- Update: `200 OK` with the saved room or program object.
- Deletion: `204 No Content`.
- Unknown household, room, or program: `404 Not Found`.
- Malformed or invalid input, including schedule collisions: `400 Bad Request`.
- An identifier already present during creation, or a controller IP assigned to another room: `409 Conflict`.

Deleting a room removes its server assignment. Later `/get_state` requests from its former controller receive `404` unless that IP has been reassigned. The controller follows the five-hour API-outage fallback; deleting the room does not remotely stop or reset the hardware.

### 6.6 Validation and persistence

Validation rules:

- Identifiers must be nonempty and unique within their scope.
- Controller IP addresses must be valid IPv4 addresses; sensor MAC addresses must use six hexadecimal octets separated by colons. MAC comparison is case-insensitive, and saved values use a consistent format.
- A controller IP address may be assigned to only one room across all households.
- Temperatures must be finite numeric values; deltas must be finite, nonnegative numeric values.
- Program start times must use valid 24-hour UTC `HH:MM` format. Check collisions against the applicable UTC weekdays using the priority and collision rules in section 5.
- If supplied, `days` must contain one or more unique, supported UTC weekday names.
- Timezones must be valid timezone identifiers.
- Invalid updates must leave the previously stored configuration intact.

No additional temperature bounds or restrictions on sharing a sensor across rooms are specified. The controller IP uniqueness rule remains mandatory.

Store household, room, and program configurations in JSON files on the Raspberry Pi. Configuration must survive API and Raspberry Pi restarts. File organization is an implementation detail; it must support loading every household for `/get_house_list` and controller IP lookup for `/get_state`.

Write configuration changes atomically and coordinate simultaneous updates so a partially written file or concurrent write does not corrupt saved data. Report success only after the updated configuration has been saved. Rebuild or update the controller-to-room lookup after configuration changes.

Error response format:

```json
{
  "error": {
    "code": "invalid_config",
    "message": "A program start time must use HH:MM format."
  }
}
```

### 6.7 Controller management and commissioning routes

The Raspberry Pi proxies the controller interface from section 4.6. Control-state polling through `/get_state` retains its existing four-field response and once-per-minute cadence. Management is a separate on-demand path and does not require waiting for a temperature-state poll.

| Method and route | Operation |
| --- | --- |
| `GET /households/{household_id}/rooms/{room_id}/controller/status` | Retrieve the assigned controller's live status and pending management operation. |
| `PATCH /households/{household_id}/rooms/{room_id}/controller/config` | Validate and submit a controller settings patch; coordinate IP and sensor changes with the room assignment. |
| `POST /households/{household_id}/rooms/{room_id}/controller/restart` | Submit a restart preserving saved configuration. |
| `POST /households/{household_id}/rooms/{room_id}/controller/retry` | Retry an unresolved operation using its original command identifier. |
| `GET /controllers/setup/status` | Retrieve status from the fixed commissioning address `192.168.0.230` on the LAN. |
| `PATCH /controllers/setup/config` | Configure a reachable default-address controller on the LAN. |
| `POST /controllers/setup/restart` | Restart that commissioning controller. |
| `POST /controllers/setup/retry` | Retry an unresolved commissioning operation with the same identifier. |

The setup proxy routes are for devices already reachable from the Raspberry Pi LAN. A new controller's initial access-point setup is performed on its embedded web page, since a browser connected to that isolated Wi-Fi network cannot assume the Raspberry Pi is reachable.

Status responses use `{ "reachable": true, "status": { ... }, "pending": null }`. If no current device response is available, use `reachable: false`, `status: null`, and a safe error description, preserving pending-operation metadata where applicable. Previously displayed measurements must be labeled as stale or last known rather than presented as live.

Controller settings requests use a plain patch object with the fields listed in section 4.6. The Raspberry Pi assigns a unique command identifier and verifies device identity. A successful delivery returns `202` and `{ "accepted": true, "confirmed": false, "command_id": "...", "restart_required": true, "pending": { ... } }`. Acceptance does not prove the device restarted or joined the new network. Confirm completion through a subsequent status response with matching device/command identifiers, the requested nonsecret settings, and `restart_pending: false`.

Persist unresolved operations outside the household JSON directory so they survive API restarts without being parsed as household files. Reserve old and proposed IPs across households until the result is known. Reject conflicting assignment changes or deletion/replacement of the affected room while an operation is unresolved; preserve unrelated rooms and temperature programs. A controller sensor change must update the room's sensor MAC, preventing the next `/get_state` response from undoing it.

Validate room and device settings before delivery. Store the pending operation and coordinated room assignment before delivering a settings command. A definitive device rejection can restore the previous assignment fields. A transport timeout has an unknown outcome: keep the pending operation/reservations and expose verification/retry instead of claiming rollback or completion. Retrying must reuse the original command identifier so an already executed restart is not repeated. Remove pending secrets once the operation no longer needs them; never include passwords in browser responses, errors, or logs.

Use fixed controller port/path targets derived from saved room IPs or the fixed commissioning address. Do not offer arbitrary URL forwarding or follow HTTP redirects. Bound connection/read times, response size, and JSON parsing. A controller may be busy with its regular API poll, so allow an appropriate bounded management response time. Ordinary offline requests fail visibly rather than becoming indefinitely queued restart commands.

Use `400` for invalid settings, `404` for an unknown room, `409` for conflicting assignments/device identity or an unresolved operation, `502` for unreachable/invalid controller responses, and `504` for timed-out communication. Mutation errors must indicate an unresolved outcome when one exists. All existing LAN access restrictions apply.

## 7. React frontend requirements

The frontend must enable users to:

1. Add a household to the system.
2. Select an existing household and view its rooms and their associated configuration.
3. Add or remove rooms within a household.
4. Add or remove room temperature programs and edit their start times and target temperatures.
5. Edit a room's controller IP address and sensor MAC address.
6. Choose whether each program applies every day or only on selected weekdays.
7. Edit room-level `negative_delta` and `positive_delta` values.
8. Open a controller status window for each room/zone and view the live fields defined in section 4.6.
9. Request a restart of the selected controller and distinguish accepted, pending, confirmed, unreachable, and unconfirmed results.
10. Edit all persistent controller settings through a dedicated controller panel, including Wi-Fi, IP/subnet/gateway/DNS, API host/port, and sensor MAC.
11. Access instructions and commissioning controls for new default-address controllers, including entirely web-based initial Wi-Fi setup.

Use `/get_house_list` and `/get_config` to load households and their configuration. Create a household through `/set_config` with its complete initial configuration, including an empty `rooms` array when appropriate. Use the room and program routes in section 6.5 for ordinary additions, edits, and removals so unrelated data is preserved.

When explicitly saving a complete household configuration through `/set_config`, send the full current snapshot. The frontend must not send a partial room or program edit to this replacement endpoint.

Show validation and API errors, indicate whether saving succeeded, and preserve unsaved edits after a failed request. Display household and room names and make the configured timezone visible when editing programs. Show the effective local start times and weekdays, and make clear that these shift with daylight-saving changes to preserve UTC timing.

Convert local time and weekday selections to UTC when creating a program or explicitly editing its schedule. Convert API values back to local display when loading them. Saving an unrelated field, such as a temperature target, must preserve the existing UTC schedule. See section 11 for the conversion convention.

The room-assignment form continues to identify which controller IP belongs to a room. The dedicated controller settings panel must change the device's actual network settings and coordinate any resulting room IP/sensor assignment through section 6.7. Make this distinction clear in the interface. Ordinary room sensor-assignment edits still propagate on the next successful `/get_state` poll.

Status windows must refresh while open, show data/loading/error states, and avoid carrying a previous room's readings into another room's window. Display sensor and API data ages and fresh/stale status; an offline controller must not display cached readings as live. Abort obsolete requests when switching rooms or closing a panel.

Before restarting or applying settings that restart the controller, show the affected controller/room and that heating starts ON afterward. Preserve unsaved inputs on failures. A blank password field preserves the saved password unless the user explicitly selects an open network. Never prefill or display a saved password.

Show commissioning instructions even when no household exists: connect to `BMS-Setup-<device suffix>`, open `http://192.168.0.230`, enter the household Wi-Fi and a unique controller IP, then return to the main application. Explain that only one default-address controller can be on the LAN at a time.

Deleting households, temperature history, and manual heating overrides remain outside scope. Live temperatures and heater status are now required by `new_features.md`.

## 8. Raspberry Pi installation and startup

The installation script must:

- Target the Raspberry Pi 4 Model B.
- Support the latest stable Raspberry Pi OS desktop release available when the installation is prepared, documenting the exact release tested.
- Install Python, Flask, and the dependencies needed to run the backend, plus the build and serving dependencies needed for the React frontend.
- Prepare the Flask API to listen on port `2026`.
- Build and serve the React frontend through its own server on port `3000`.
- Initialize JSON configuration storage without overwriting existing household data.
- Install or provide two `.desktop` files, one for each server, so both launch on login.

Document prerequisite OS and package versions, application paths, startup commands, and how to view logs. Re-running the installation script must preserve existing household configuration.

Configure the Raspberry Pi hostname as `BMS` and document how that name resolves on the local network. Expected local URLs are `http://BMS:3000` for the frontend and `http://BMS:2026` for the API where the network resolves `BMS`. An IP-based API host must remain configurable on controllers.

Since the frontend and API use different ports, configure the API to allow requests from the configured local frontend origin. Use the actual hostname or IP used to open the frontend when defining allowed origins.

The installation must preserve the requirement that both applications are available only on the shared local network. Authentication is not required. Do not configure public exposure as part of installation.

Login startup is the requested behavior. Install the `.desktop` files in the selected user's desktop autostart location and verify both applications start on login. Starting the applications before user login is outside this requirement.

## 9. Acceptance criteria

| ID | Verification | Expected result |
| --- | --- | --- |
| AC-01 | Add two households using the frontend. | Both appear in the household list with separate configurations. |
| AC-02 | Save a configuration with a new household identifier. | The API creates the household. |
| AC-03 | Retrieve an existing household configuration. | The API returns its stored rooms, device assignments, and programs. |
| AC-04 | Add and remove rooms. | The selected household's configuration reflects the changes. |
| AC-05 | Add, edit, and remove daily and weekday temperature programs. | The saved room configuration reflects each operation and selected weekdays. |
| AC-06 | Change a controller IP or sensor MAC through the controller settings panel. | The device and server room assignment are coordinated; completion is verified at the new address without requiring serial configuration. |
| AC-07 | Request `/get_state` from a configured controller's fixed IP. | The response contains that room's current target, both deltas, and sensor MAC. |
| AC-08 | Observe controller requests over several minutes. | The controller polls `/get_state` once per minute. |
| AC-09 | Boot a controller before its first successful API response. | Heating starts ON; target and deltas begin at `23.0`, `0.1`, and `0.1 °C`, and the saved sensor MAC is loaded. |
| AC-10 | Supply format-5 readings from the assigned RuuviTag and another tag. | The controller decodes the assigned tag's temperature using the supplied conversion and ignores the other tag. |
| AC-11 | Move measured temperature across the control thresholds and test equality. | Pin `4` switches heating on below `target - negative_delta`, off above `target + positive_delta`, and retains state at and between the thresholds. |
| AC-12 | Configure Wi-Fi, fixed IP and network settings, API host/port, and sensor through prompted serial commands; power-cycle. | The controller retains its saved local configuration. |
| AC-13 | Send `reset` through the Arduino serial monitor after configuring the controller. | Hardware restarts, stored Wi-Fi/IP/API/sensor settings remain intact, heating starts ON, and runtime target and deltas return to defaults. |
| AC-14 | Run the installer and then log into the Raspberry Pi desktop. | The Flask API on `2026` and React frontend on `3000` launch through their respective `.desktop` files. |
| AC-15 | Exercise daily and weekday UTC schedule boundaries and cross local and UTC midnight. | The next successful poll returns the most recent applicable UTC target; neither midnight resets it. |
| AC-16 | Apply nondefault target, deltas, and a new sensor MAC through the API; reboot the controller. | The new sensor MAC persists, target and deltas return to defaults, and heating starts ON. |
| AC-17 | Set different deltas for two rooms. | Each controller receives its own room's deltas. |
| AC-18 | Send `help` through serial. | The firmware lists the available single-token commands and their purposes. |
| AC-19 | Save configuration, restart the API and Raspberry Pi, and retrieve it. | JSON-backed household, room, and program configuration is preserved. |
| AC-20 | Submit invalid configuration or a duplicate controller IP. | The request fails with the documented error, and stored configuration remains intact. |
| AC-21 | Operate Bluetooth temperature acquisition while polling over Wi-Fi. | Both functions continue to work on the actual Arduino hardware with the recorded firmware and library versions. |
| AC-22 | Access the frontend and API from the intended local network. | The applications work without authentication and are deployed for local network access only. |
| AC-23 | Stop valid readings from the assigned sensor while heating is OFF; continue sending advertisements from another sensor. | Pin `4` becomes HIGH after more than 30 seconds without a valid assigned-sensor reading; unrelated advertisements do not postpone the timeout. |
| AC-24 | Restore a valid assigned-sensor reading above `target + positive_delta` after a timeout. | Normal control resumes and pin `4` becomes LOW. |
| AC-25 | Configure daily and day-specific programs at the same UTC start time. | On the specified UTC weekday, the day-specific target takes priority; on other UTC weekdays, the daily target applies. |
| AC-26 | Stop valid API responses after applying a nondefault state while sensor readings remain available. | The controller retains that runtime state before five hours; at five hours it uses target `23.0 °C` and deltas `0.1 °C`, preserving the sensor MAC and network settings. |
| AC-27 | Continue failed or invalid polls, then restore a valid API response. | Failures do not refresh the five-hour timer; the valid response immediately applies its state and resets the timer. |
| AC-28 | Stop both API responses and assigned-sensor readings. | The sensor timeout forces heating ON after more than 30 seconds; the five-hour temperature fallback does not override the missing-sensor rule. |
| AC-29 | Send `factory-reset` through serial. | Local credentials, custom network settings, and sensor MAC are cleared; default IP becomes `.230` and API becomes `BMS:2026`; hardware restarts with heating ON and web setup available; Raspberry Pi JSON configuration remains intact. |
| AC-30 | Add, patch, and delete rooms and programs through the additional API routes. | Each operation changes only its intended entity, returns the documented status, and preserves unrelated data. |
| AC-31 | Change one field of a program through `PATCH` and then load the household. | The supplied field changes and omitted fields remain unchanged. |
| AC-32 | Replace an existing household through `/set_config` with a complete snapshot omitting a former room or program. | The omitted entity is removed; a partial snapshot missing required collections is rejected. |
| AC-33 | Advance a household timezone from UTC+02:00 to UTC+03:00 with a program at `05:00 UTC`. | The program stays at `05:00 UTC`; its local display changes from `07:00` to `08:00`, without rewriting the stored schedule. |
| AC-34 | Move the timezone offset back across a repeated local clock interval. | Stored UTC recurrence remains unchanged, each UTC occurrence is considered once, and the active target does not revert because the local clock repeats. |
| AC-35 | Create a local Monday `00:30` program while the timezone offset is UTC+02:00. | The stored schedule is Sunday `22:30 UTC`; after an offset change to UTC+03:00, its local display is Monday `01:30`. |
| AC-36 | Restart the API after a daylight-saving change and save an unrelated program field. | Stored UTC times and weekdays remain unchanged, and the active target is reconstructed from UTC occurrences. |
| AC-37 | Open the controller status window for each of two rooms. | Each displays its own controller's connection/IP, heater, temperatures, settings, firmware/BLE, and measurement/API ages; switching rooms cannot show another device's status. |
| AC-38 | Disconnect a controller while its status window is open. | The interface reports unreachable or stale data rather than presenting previous values as live. |
| AC-39 | Restart a reachable controller from its room panel. | Acceptance is shown first; completion is verified from status after reboot; saved settings remain intact, heating starts ON on pin `4`, and runtime temperature settings reset to defaults. |
| AC-40 | Update Wi-Fi/network/API/sensor settings from the web panel. | The entire candidate is validated before saving, changes persist across reboot, and the room's IP/sensor assignment stays consistent. |
| AC-41 | Lose the response after the device accepts an IP change, then retry. | The operation remains unconfirmed until verified; both addresses stay reserved, and duplicate command delivery does not cause repeated reboot. |
| AC-42 | Read status, configuration, command results, and error responses after entering a Wi-Fi password. | The password is never returned; status provides only whether one is set. |
| AC-43 | Boot a factory-new controller with no serial connection and no Raspberry Pi connection. | Its setup Wi-Fi network and embedded page at `192.168.0.230` allow complete first-time Wi-Fi/network configuration. |
| AC-44 | Commission two new controllers sequentially. | Each initially uses `.230`; each receives a unique operating IP before the next joins the LAN. Existing saved addresses are preserved. |
| AC-45 | Keep Wi-Fi disconnected during normal configured operation for ten continuous minutes. | The controller automatically restarts with saved settings intact; a reconnection before the deadline restarts that timer. |
| AC-46 | Stop all BLE advertisements during normal operation for ten minutes. | The controller automatically restarts; assigned-sensor heating has already switched ON after more than 30 seconds without a valid reading. |
| AC-47 | Stop assigned-sensor readings but continue advertisements from another BLE device. | Heating becomes ON after the 30-second sensor timeout; unrelated advertisements refresh only the Bluetooth recovery timer and prevent the ten-minute BLE restart. |
| AC-48 | Leave an unconfigured controller in web setup for more than ten minutes. | Setup remains available without a restart loop, and heating safeguards continue. |
| AC-49 | Simulate a stalled radio call and elapsed-time rollover. | Recovery deadlines still execute independently of the blocked main loop and remain correct across rollover. |
| AC-50 | Enter an invalid SSID/password, subnet/gateway combination, API port, or sensor MAC through web configuration. | The complete candidate is rejected without partial persistence or an incorrect success message. |
| AC-51 | Restart the API with an unresolved controller operation. | Pending state and old/new IP reservations survive; unrelated room/program edits remain possible and conflicting edits are rejected. |
| AC-52 | Enter incorrect Wi-Fi credentials through initial web setup. | The controller provides a setup recovery path so credentials can be corrected without serial access. |

## 10. Decision record

| Topic | User decision and resulting specification |
| --- | --- |
| API outages | Retain the latest runtime settings for five hours, then use the default target and deltas. The saved sensor MAC and local network settings remain intact. See section 4.3. |
| Daylight-saving transitions | Preserve UTC timing. Stored UTC start times and weekdays remain fixed; displayed local times shift with the timezone offset. See sections 5 and 11. |
| Frontend/backend coordination | Additional API routes are authorized. Section 6.5 defines operations for individual rooms and programs; `/set_config` retains complete replacement semantics as a selected implementation decision. |
| Reset commands | `reset` only restarts the hardware. `factory-reset` clears local configuration and restores defaults before restarting. See section 4.4. |
| First-time Wi-Fi setup | Must work entirely through a web page; initial serial Wi-Fi configuration is not acceptable. Provide a temporary controller setup access point at `192.168.0.230`. See section 4.5. |
| Default controller address | Use `192.168.0.230` for commissioning; configure devices one at a time and retain unique operating addresses. |
| Automatic recovery | Ten continuous minutes for Wi-Fi loss and ten minutes without **any** BLE advertisement. The separate 30-second assigned-sensor heating rule remains unchanged. See section 4.7. |
| Heater output revision | Use **pin `4`**, matching the current firmware, as confirmed on 2026-10-23. This supersedes the earlier pin `6` assignment; active-HIGH behavior is retained. |
| Web controller management | Provide per-zone live status, restart, and all persistent settings previously available through serial. Accepted changes must be verified after reboot. See sections 4.6, 6.7, and 7. |

The prior decisions on startup heating ON, active-HIGH pin `4`, the 30-second sensor timeout with heating ON, day-specific priority, room-level deltas, the shared network, persistent serial and web network configuration, Flask/JSON storage, `BMS` hostname, and local access without authentication remain in force.

## 11. UTC scheduling convention

The user confirmed that schedules must preserve UTC timing. The following storage and input convention implements that decision:

1. API payloads and persisted JSON express program `start_time` and `days` in UTC. The existing field names and endpoint structures remain unchanged.
2. The frontend displays and accepts times and weekday selections in the household timezone. **Selected input convention:** use the timezone offset in effect at the time of an explicit schedule edit to convert the user's selection into a fixed UTC recurrence. Show the resulting local schedule before saving.
3. When converting a local weekday and time to UTC, adjust the weekday if the conversion crosses midnight. Omitting `days` continues to represent an event on every UTC day.
4. Once saved, UTC recurrence remains fixed across daylight-saving changes, API or Raspberry Pi restarts, and changes to the display timezone. Such changes update the local presentation rather than rewriting stored schedules.
5. Explicitly changing a program's start time or applicable days creates the corresponding updated UTC schedule. Changing only its target temperature or another unrelated field must preserve the stored UTC schedule.
6. Resolve the active program from ordered UTC occurrences. Retain the most recent applicable target until another event occurs, and apply day-specific priority when events share a UTC start instant.

Local start dates, times, and weekdays are projections of UTC occurrences. A transition may move a displayed event onto a different local weekday when it crosses midnight. Clock jumps do not create extra events or remove a scheduled UTC occurrence. These rules close the previously pending daylight-saving clarification.
