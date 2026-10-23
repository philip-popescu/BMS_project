# BMS home heating

A local heating system implementing [specifications.md](specifications.md): a Python Flask API, React configuration interface, Arduino MKR WiFi 1010 firmware, and Raspberry Pi installation and login startup scripts.

## Applications

| Component | Location | Port |
| --- | --- | --- |
| Flask API, served by Waitress | `backend/api.py`, `backend/server.py` | `2026` |
| React interface | `frontend/src/` | `3000` |
| Production frontend server, served by Waitress | `backend/frontend_server.py` | `3000` |
| Controller sketch | `firmware/BMSController/` | Arduino serial: `115200` baud |
| Pi setup and launchers | `scripts/`, `deployment/` | Both servers launch on desktop login |

The interface manages households, rooms, controller settings, RuuviTag assignments, room deltas, and UTC-based temperature programs. Room cards show scheduled targets. Each room also has a **Controller status** window with live temperature, heater, network, BLE, and API information, plus settings and restart controls.

## Run locally on Windows

Use Python 3.10+ and Node 20.19+ or 22.12+. From the project root:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.lock
npm.cmd --prefix frontend ci
npm.cmd --prefix frontend run build
.\.venv\Scripts\python.exe -m backend.server
```

In another terminal:

```powershell
.\.venv\Scripts\python.exe -m backend.frontend_server
```

Open `http://127.0.0.1:3000`. Create a household, add its rooms, and add programs. A fresh installation contains no sample households. Both servers bind to loopback by default. Windows launch commands do not automatically read `.env`; set any desired variables with `$env:BMS_TIMEZONE = 'Europe/Bucharest'`, for example, before launching.

For frontend development, run `npm.cmd --prefix frontend run dev` instead of the production frontend server. Both use port `3000`, so run only one at a time. Keep the API running separately.

## Install on Raspberry Pi

Use the latest stable **Raspberry Pi OS desktop** installation for Pi 4 Model B. Copy this entire project to the Pi, connect it to the shared LAN, and run as the user who will log into the desktop:

```bash
bash scripts/install.sh
```

The script uses `sudo` for system packages and the `BMS` hostname, creates a Python virtual environment, installs locked application dependencies, builds React, initializes JSON storage, and installs both desktop autostart entries. Existing `.env` and household data are preserved on subsequent runs. It records the actual OS and dependency versions in `logs/installation-versions.txt`.

Start both applications now in separate terminals:

```bash
bash scripts/start-api.sh
bash scripts/start-frontend.sh
```

On later desktop logins they start automatically. Launchers prevent duplicate instances using `flock`. View errors and startup messages in `logs/api.log` and `logs/frontend.log`.

Open `http://BMS:3000` or `http://bms.local:3000`. A numeric LAN address, such as `http://192.168.1.10:3000`, also works. The installer enables Avahi for the `.local` hostname. Bare `BMS` resolution depends on the router's local DNS; configure a local DNS entry or use the Pi's numeric IP in the controller's `set-api` command if needed. Reserve the Pi's address in the router when using a numeric API host.

If dependencies are already installed, `--skip-system-packages` skips apt and hostname changes. `--no-autostart` skips installing login entries. A Pi without a default network route can be installed by supplying the LAN subnet:

```bash
BMS_LAN_CIDR=192.168.1.0/24 bash scripts/install.sh
```

The project stays at its installation path; update the autostart entries by rerunning the installer if you move it. The installer does not flash any controllers or start servers before desktop login.

## Environment and storage

Pi launchers load `.env` as a shell environment file. Supported settings:

| Setting | Default / purpose |
| --- | --- |
| `BMS_BIND_HOST` | `127.0.0.1` locally; installer sets `0.0.0.0` for LAN service. |
| `BMS_ALLOWED_NETWORKS` | Private and loopback ranges locally; installer narrows this to loopback and the detected LAN subnet. Direct peer addresses are checked on both servers. |
| `BMS_ALLOWED_ORIGINS` | Comma-separated frontend origins; `BMS`, `bms.local`, and localhost on port `3000`. Numeric addresses in allowed networks on port `3000` are also accepted. |
| `BMS_TIMEZONE` | Raspberry Pi's configured timezone, otherwise UTC. Used as the default for new households. |
| `BMS_DATA_DIR` | `data/houses` under the project root. |

Requests from outside the allowed networks are rejected; forwarded client IP headers are ignored. The API requires no authentication, as specified. No public hosting or router port forwarding is configured.

Each household is stored in a JSON file named from a hash of its identifier. A process-wide transaction lock coordinates reads and mutations, and writes use temporary files, `fsync`, and atomic replacement. Invalid requests leave existing data intact. Corrupt storage produces an error instead of silently replacing data. Pending controller operations are saved separately in `data/controllers/`; they reserve old/new IPs and survive API restarts. Back up both directories together and retain filenames. Pending settings can include a replacement Wi-Fi password until verification completes; passwords are omitted from status, browser responses, and errors, and completed operations are removed.

The four original API routes and all room/program routes in the specification are implemented. Full `/set_config` updates require complete room and program collections. Ordinary edits use `PATCH`, preserving unspecified fields. Deleting a room removes its server assignment; its controller continues with its last state for up to five hours, then uses temperature defaults.

## UTC schedules

API and stored program times and weekdays are UTC. The frontend accepts local times using the household timezone's current offset, adjusts weekdays when conversion crosses midnight, and previews the resulting fixed UTC schedule. Day-specific programs override everyday programs at a matching start time.

For example, Monday `00:30` at UTC+02:00 becomes Sunday `22:30 UTC`. It subsequently displays as Monday `01:30` at UTC+03:00. Editing only a target temperature preserves the UTC schedule. The most recent applicable program stays active across midnight and restarts until the next event; rooms with no programs use `23 °C`.

## Configure Arduino controllers

The heater output is now **pin 4, active HIGH**, as confirmed on 2026-10-23. Connect the heater control to that pin before using this firmware. Heating starts ON on every reboot; more than 30 seconds without a valid reading from the assigned sensor also forces heating ON.

Open `firmware/BMSController/BMSController.ino` in Arduino IDE and select **Arduino MKR WiFi 1010**. Install the SAMD board package and these Library Manager libraries:

| Dependency | Version used for compilation |
| --- | --- |
| Arduino SAMD Boards | `1.8.14` |
| WiFiNINA | `2.1.1` |
| ArduinoBLE | `2.1.0` |
| Arduino_SpiNINA | `0.0.2` |
| ArduinoJson | `7.4.3` |
| FlashStorage | `1.0.0` |

Concurrent Wi-Fi and BLE require **NINA firmware 3.0.1+**, WiFiNINA 2.0.0+, and ArduinoBLE 2.0.0+. The sketch checks the NINA version and reports an upgrade requirement if necessary. The supported combination is documented in [Arduino's concurrent Wi-Fi/BLE announcement](https://blog.arduino.cc/2026/03/02/you-can-now-use-wi-fi-and-bluetooth-le-simultaneously-on-arduino-nina-based-boards-heres-how/).

If ArduinoBLE reports that Bluetooth initialization failed and asks for NINA 3.0.0+, that is a generic message from its failed Bluetooth reset command; it does not tell you the installed firmware version. BMS requires **3.0.1+**. Type `status` in the BMS serial monitor to see the detected NINA version, whether its read succeeded, compatibility, and scanning state, including if you missed the startup output. It also queries the current Wi-Fi connection and displays the current IP address when connected, along with saved SSID, static IP, subnet, gateway, and pending network changes. The numeric Wi-Fi status at the last firmware check is labeled separately as a snapshot. An unreadable version indicates a communication problem; it must not be mistaken for outdated firmware. A successfully read unsupported version skips radio initialization. Serial configuration and heater timeout control continue in either case.

BMS initializes Arduino's shared NINA SPI driver, allows an additional **1.5 seconds** for module startup, and requests Wi-Fi status before reading the firmware version, following the sequence used by the working standalone probe. If that check fails, BMS resets only NINA, waits for startup, and checks again; continued failures retry every 10 seconds. The TC5 heater timer remains active during module startup and recovery, and saved settings are preserved. On the user's controller, both `NinaProbe` and the revised BMS sketch have read firmware **3.0.1** and successfully started BLE scanning. Wi-Fi/API polling alongside reception from the assigned sensor still needs verification on the controller.

To update the module in Arduino IDE 2:

1. Close the Serial Monitor and open **Tools > Firmware Updater**.
2. Select the **MKR WiFi 1010** and its port, check for updates, and install NINA **3.0.1 or newer**.
3. Confirm WiFiNINA and ArduinoBLE are both version **2.0.0 or newer** in Library Manager, and that Arduino_SpiNINA is installed.
4. Upload `BMSController` again and re-enter its Wi-Fi, static network, API, and sensor settings. The updater overwrites the running sketch, and replacement sketch uploads erase FlashStorage configuration.

These update steps follow [Arduino's Firmware Updater instructions](https://support.arduino.cc/hc/en-us/articles/360013896579-Use-the-Firmware-Updater-in-Arduino-IDE). If NINA already reports 3.0.1+ but initialization still fails, confirm the selected board and actual library paths in verbose compilation output and power-cycle the board. Run `status`, then `debug-radio` and provide their complete output. The latter enables ArduinoBLE's HCI command trace and immediately retries failed initialization without erasing configuration; `reset` disables tracing. A supported version alone does not prove the radio hardware or SPI communication is working.

If an upload reports `No device found on COM...`, close the updater, Serial Monitor, and other applications using the port. Press RESET twice quickly; the fading LED indicates the bootloader is waiting for an upload. Select the newly detected port before retrying, following [Arduino's bootloader reset instructions](https://support.arduino.cc/hc/en-us/articles/5779192727068-Reset-your-board). The bootloader and running sketch can use different ports; during the recovery on this controller they were **COM5** and **COM7**, respectively. Check the current port each time. Heating control is paused while the bootloader or updater is running.

Recovery on **2026-10-11** succeeded using Arduino Firmware Uploader 2.4.1 to reinstall **NINA 3.0.1**, followed by a verified upload of `BMSController`. Immediately afterward, BMS still reported a blank version and Wi-Fi status **255**. Disconnecting **all power sources for 10 seconds**, including USB and any battery/external supply, then reconnecting restored the firmware response and active BLE scanning. The final `status` confirmed **3.0.1**, supported compatibility, and scanning active. Saved Wi-Fi, static IP, and sensor settings were cleared by the sketch uploads and require reconfiguration; without an assigned sensor, heating remains ON.

For a persistently blank version, first disconnect every power source, including USB and any battery, then reconnect and run `status` again. If it remains blank, use the temporary [`NinaProbe` sketch](firmware/NinaProbe/NinaProbe.ino) to isolate the radio from BMS: select MKR WiFi 1010, upload it, and open Serial Monitor at **115200 baud** with newline termination. It prints the Wi-Fi status and firmware response without the BMS timer or FlashStorage. Type `recover` to reset NINA through Arduino's shared SPI driver and repeat the check after an additional 1.5-second wait. If a version is readable, type `ble` to trace Bluetooth initialization and test scanning. Provide the complete startup and command output, including where a call stops if it hangs.

The probe is a temporary diagnostic and keeps pin **4 HIGH** throughout; it does not regulate heating or contact the API. Uploading it erases saved BMS FlashStorage settings. Restore `BMSController` and reconfigure it after the test. A failure in the standalone probe points toward the module, its firmware, power, or the SPI/library path; a successful probe with a failing BMS sketch calls for further investigation of BMS integration. Neither result alone establishes that the hardware is defective.

### First-time setup through the web

1. Upload the updated `BMSController` sketch. With no saved Wi-Fi credentials it creates an open Wi-Fi network named `BMS-Setup-` followed by a device suffix.
2. Connect a phone or computer to that network and open **http://192.168.0.230**. The page is embedded in the controller and works without Internet access or the Raspberry Pi.
3. Enter the household Wi-Fi credentials, a unique operating IP, subnet, gateway, DNS, API host/port, and optionally the initial sensor MAC. Use a numeric Pi address if your router does not resolve `BMS`.
4. Save and allow the controller to restart, then rejoin your household Wi-Fi. If the new credentials cannot establish the initial connection, use the setup network again to correct them.
5. In the main app, assign that IP and RuuviTag to a room. Its **Controller status** window provides live status, all persistent controller settings, and restart controls.

Default network values are **192.168.0.230**, subnet **255.255.255.0**, gateway/DNS **192.168.0.1**, and API **BMS:2026**. Commission only one default-address controller on the LAN at a time. Existing saved addresses are preserved. The main app's **Set up a controller** action also manages a default-address device after it has joined the same LAN as the Pi. A computer connected to the controller's isolated setup Wi-Fi uses the embedded page directly.

### Controller status and settings

The main app contacts controllers through the Pi API and the controller's local HTTP server on port **80**. Open a room's **Controller status** window to see current Wi-Fi/IP, heater state, assigned-sensor temperature and age, active target/deltas, last valid API update, BLE reception age, NINA firmware, and uptime. Offline readings are marked as last known. Status refreshes while the window is open.

The settings tab changes Wi-Fi, IP/subnet/gateway/DNS, API host/port, and assigned sensor. The password is write-only: leaving it untouched preserves it; explicitly choosing an open network clears it. Web settings are saved and followed by a restart. Device IP and sensor changes are coordinated with the room assignment; ordinary room assignment edits identify a controller without changing its network configuration.

Commands first show **accepted**, then await confirmation after restart. An interrupted response can leave an operation **unconfirmed** even if the device applied it. Refresh status or use **Check and retry same command**; the original command ID prevents duplicate reboots. Old and new IPs remain reserved until verification, and conflicting room assignment changes are blocked. Changing Wi-Fi or API settings can make the device unreachable from the Pi; use its setup recovery page to correct a failed initial connection.

During normal configured operation, the controller restarts after more than **10 minutes** without Wi-Fi connectivity or without **any Bluetooth advertisement**. Unrelated advertisements refresh only that Bluetooth recovery timer; they never refresh the separate 30-second assigned-sensor reading timer. Automatic recovery is suspended during web provisioning. Recovery deadlines run in the heater timer so a stalled radio call cannot disable them. An API outage with working Wi-Fi and BLE still follows the existing five-hour temperature fallback.

Some connectable advertisements do not reach ArduinoBLE's normal discovery callback. The firmware also observes advertising-report events through its public debug-stream interface, using the trace format tested with **ArduinoBLE 2.1.0**. This observer is silent during normal operation; `debug-radio` also forwards tracing to Serial. Keep the pinned library version or recheck this format and advertisement recovery behavior when upgrading it. Commission this behavior with real advertisements on the controller.

### Serial configuration

Upload the sketch, open the serial monitor at `115200` baud, and select newline or CRLF termination. Type `help`. Commands prompt for each field:

| Command | Operation |
| --- | --- |
| `status` | Show current Wi-Fi/IP, saved network settings, pending restart, radio/BLE status, active target/deltas and their source, assigned sensor, and latest temperature with age and fresh/stale status. |
| `debug-radio` | Enable BLE HCI tracing and retry failed initialization; tracing ends on restart. |
| `set-wifi` | Enter SSID and password; blank password selects an open network. |
| `set-ip` | Enter static IP, subnet, gateway, and DNS. Blank subnet uses `255.255.255.0`; blank DNS uses the gateway. |
| `set-api` | Enter IP/hostname and port. Blank values use `BMS` and `2026`. |
| `set-sensor` | Enter the assigned RuuviTag's colon-separated MAC address. |
| `cancel` | Abandon a prompted update before saving. |
| `reset` | Restart while retaining saved configuration. |
| `factory-reset` | Clear saved configuration, restore defaults, and restart. |

After setting Wi-Fi or static network parameters, type `reset` to apply them. Pending network changes pause API polling until restart. Set the same controller IP in the frontend's room settings. Controller addresses must be unique across households. The API uses the direct peer IP for identification.

Setting a static IP alone does not join Wi-Fi: both `set-wifi` and `set-ip` must be saved. The first connection attempt runs when the main loop starts, with further attempts at the one-minute API poll interval. Missing configuration now produces an explicit serial message. If the controller does not respond to ping, run `status` and check **Wi-Fi: connected** and **Current IP address**, rather than relying on the configured IP. Confirm the configured subnet/gateway and the computer's LAN match. A pending restart is reported; type `reset` after configuring the network. Firmware uploads erase saved Wi-Fi and network settings.

Subnet masks use separate validation from host addresses: `255.255.255.0` is accepted explicitly or by leaving the subnet prompt blank. Masks must have contiguous bits and allow usable host addresses; the controller must share the gateway's subnet and cannot use that subnet's network or broadcast address.

The heater is active HIGH on pin `4` and starts ON without waiting for USB serial, Wi-Fi, or sensor acquisition. A dedicated SAMD21 **TC5** interrupt evaluates control every approximately 100 ms, independent of blocking radio calls. TC5 is reserved by this sketch; do not add another library that takes over that timer.

Heating switches ON below `target - negative_delta` and OFF above `target + positive_delta`, holding its previous state at and between the boundaries. More than 30 seconds without a valid assigned-sensor reading forces heating ON. At five hours without a valid API response, target and deltas return to `23`, `0.1`, and `0.1 °C`, preserving sensor and network configuration. Polling continues once per minute; a valid response restores API settings immediately.

Only the API-supplied sensor MAC is written to flash, and only when it changes. Runtime target and deltas return to defaults on reboot. The decoder reads bounded manufacturer data rather than fixed advertisement offsets, handles signed format-5 temperatures, and rejects the unavailable-temperature sentinel according to [Ruuvi RAWv2 documentation](https://docs.ruuvi.com/communication/bluetooth-advertisements/data-format-5-rawv2).

With Serial Monitor open at **115200 baud**, each accepted API response prints its configuration JSON after applying it. Valid readings from the assigned Ruuvi sensor print its MAC and temperature in Celsius with three decimal places. Sensor logging prints the first reading after each assignment and then at most once per second; every valid advertisement still updates heater control and the sensor timeout. Example output:

```text
API config received and applied: {"target_temperature":23.0,"negative_delta":0.1,"positive_delta":0.1,"sensor_mac":"AA:BB:CC:DD:EE:FF"}
Ruuvi sensor AA:BB:CC:DD:EE:FF temperature: 22.895 C
```

Run `status` to inspect the active target/deltas, whether they come from a current API response or defaults, the assigned sensor MAC, and its latest received temperature and age. After the sensor timeout, the last temperature is marked **stale**. Changing the sensor assignment clears that diagnostic reading, so an old sensor's temperature cannot appear under the new assignment. Invalid API responses and unrelated or malformed sensor advertisements do not produce accepted-data logs. Receipt logs and diagnostic readings are runtime-only; they do not add flash writes.

Compile with Arduino CLI using the pinned profile, which resolves its platform and libraries:

```bash
arduino-cli compile --profile mkrwifi1010 firmware/BMSController
```

The controller-management build uses **112,960 bytes of flash (43%)** and **9,424 bytes of static RAM (28%)** with the listed versions. FlashStorage configuration survives power cycles but is erased when a replacement sketch is uploaded, as described in [FlashStorage's documentation](https://github.com/cmaglie/FlashStorage#faq). Reconfigure the controller through its setup page after firmware uploads. The setup network uses WiFiNINA's supported custom-IP access-point mode, as illustrated in [Arduino's AP web-server example](https://github.com/arduino-libraries/WiFiNINA/blob/master/examples/AP_SimpleWebServer/AP_SimpleWebServer.ino).

## Tests and verification

Backend and frontend tests:

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest -q
npm.cmd --prefix frontend test
npm.cmd --prefix frontend run build
```

Firmware tests execute the **same C++ headers** used by the sketch. On Linux, compile the host bridge before pytest:

```bash
mkdir -p .tools
g++ -shared -fPIC -O2 tests/firmware_bridge.cpp -o .tools/control.so
python -m pytest -q
```

On Windows, the optional Zig compiler can create the bridge:

```powershell
.\.venv\Scripts\python.exe -m pip install ziglang
$env:ZIG_GLOBAL_CACHE_DIR = "$PWD\.tools\zig-cache"
$env:ZIG_LOCAL_CACHE_DIR = "$PWD\.tools\zig-local-cache"
.\.venv\Scripts\python.exe -m ziglang c++ -shared -O2 -Wno-nullability-completeness tests\firmware_bridge.cpp -o .tools\control.dll
.\.venv\Scripts\python.exe -m pytest -q
```

Without the bridge, pytest explicitly skips the firmware header tests. Coverage includes CRUD and atomic rejection, cross-household IP uniqueness, concurrent updates, disk persistence, local access and CORS, UTC/DST boundaries, strict heater thresholds, sensor/API/recovery timeouts, timer rollover, Ruuvi decoding, network/settings validation, bounded management HTTP parsing, controller handover/retry/reservations, and password redaction. Frontend tests also exercise controller patches and operation messages. Installer and launchers are checked with Bash syntax validation.

Local verification does not replace commissioning on a Raspberry Pi and connected Arduino. Verify hostname resolution, desktop login startup, web provisioning, persisted settings, concurrent Wi-Fi/BLE/HTTP operation, remote restart and network handover, recovery after outages, actual sensor reception, and physical pin-4 output on the devices. The 2026-10-23 changes were compiled and tested locally; no controller upload was performed. Browser surfaces were unavailable, so no visual or interactive browser inspection was performed.
