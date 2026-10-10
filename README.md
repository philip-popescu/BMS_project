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

The interface manages households, rooms, controller IPs, RuuviTag assignments, room deltas, and daily or weekday temperature programs. Targets shown are **scheduled targets**, not live temperature measurements or heater telemetry.

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

Each household is stored in a JSON file named from a hash of its identifier. A process-wide transaction lock coordinates reads and mutations, and writes use temporary files, `fsync`, and atomic replacement. Invalid requests leave existing data intact. Corrupt storage produces an error instead of silently replacing data. Back up the entire `data/houses/` directory and retain filenames when restoring it.

The four original API routes and all room/program routes in the specification are implemented. Full `/set_config` updates require complete room and program collections. Ordinary edits use `PATCH`, preserving unspecified fields. Deleting a room removes its server assignment; its controller continues with its last state for up to five hours, then uses temperature defaults.

## UTC schedules

API and stored program times and weekdays are UTC. The frontend accepts local times using the household timezone's current offset, adjusts weekdays when conversion crosses midnight, and previews the resulting fixed UTC schedule. Day-specific programs override everyday programs at a matching start time.

For example, Monday `00:30` at UTC+02:00 becomes Sunday `22:30 UTC`. It subsequently displays as Monday `01:30` at UTC+03:00. Editing only a target temperature preserves the UTC schedule. The most recent applicable program stays active across midnight and restarts until the next event; rooms with no programs use `23 °C`.

## Configure Arduino controllers

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

For a persistently blank version, first disconnect every power source, including USB and any battery, then reconnect and run `status` again. If it remains blank, use the temporary [`NinaProbe` sketch](firmware/NinaProbe/NinaProbe.ino) to isolate the radio from BMS: select MKR WiFi 1010, upload it, and open Serial Monitor at **115200 baud** with newline termination. It prints the Wi-Fi status and firmware response without the BMS timer or FlashStorage. Type `recover` to reset NINA through Arduino's shared SPI driver and repeat the check after an additional 1.5-second wait. If a version is readable, type `ble` to trace Bluetooth initialization and test scanning. Provide the complete startup and command output, including where a call stops if it hangs.

The probe is a temporary diagnostic and keeps pin **6 HIGH** throughout; it does not regulate heating or contact the API. Uploading it erases saved BMS FlashStorage settings. Restore `BMSController` and reconfigure it after the test. A failure in the standalone probe points toward the module, its firmware, power, or the SPI/library path; a successful probe with a failing BMS sketch calls for further investigation of BMS integration. Neither result alone establishes that the hardware is defective.

Upload the sketch, open the serial monitor at `115200` baud, and select newline or CRLF termination. Type `help`. Commands prompt for each field:

| Command | Operation |
| --- | --- |
| `status` | Show current Wi-Fi connection and IP, saved network settings, pending restart, NINA firmware, and BLE scanning state. |
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

The heater is active HIGH on pin `6` and starts ON without waiting for USB serial, Wi-Fi, or sensor acquisition. A dedicated SAMD21 **TC5** interrupt evaluates control every approximately 100 ms, independent of blocking radio calls. TC5 is reserved by this sketch; do not add another library that takes over that timer.

Heating switches ON below `target - negative_delta` and OFF above `target + positive_delta`, holding its previous state at and between the boundaries. More than 30 seconds without a valid assigned-sensor reading forces heating ON. At five hours without a valid API response, target and deltas return to `23`, `0.1`, and `0.1 °C`, preserving sensor and network configuration. Polling continues once per minute; a valid response restores API settings immediately.

Only the API-supplied sensor MAC is written to flash, and only when it changes. Runtime target and deltas return to defaults on reboot. The decoder reads bounded manufacturer data rather than fixed advertisement offsets, handles signed format-5 temperatures, and rejects the unavailable-temperature sentinel according to [Ruuvi RAWv2 documentation](https://docs.ruuvi.com/communication/bluetooth-advertisements/data-format-5-rawv2).

Compile with Arduino CLI using the pinned profile, which resolves its platform and libraries:

```bash
arduino-cli compile --profile mkrwifi1010 firmware/BMSController
```

The compiled sketch uses approximately 88 KB of flash and 7 KB of static RAM with the listed versions. FlashStorage configuration survives power cycles but is erased when a replacement sketch is uploaded, as described in [FlashStorage's documentation](https://github.com/cmaglie/FlashStorage#faq). Reconfigure the controller after firmware uploads.

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

Without the bridge, pytest explicitly skips the firmware header tests. Coverage includes CRUD and atomic rejection, cross-household IP uniqueness, concurrent updates, disk persistence, local access and CORS, UTC/DST boundaries, strict heater thresholds, sensor/API timeouts, timer rollover, Ruuvi decoding, subnet mask validation, and static network checks. Installer and launchers are checked with Bash syntax validation.

Local verification does not replace commissioning on a Raspberry Pi and connected Arduino. Verify hostname resolution, desktop login startup, persistent serial settings, concurrent radio operation, actual sensor reception, and physical pin-6 output on the devices. Browser surfaces were unavailable in the implementation session, so no visual or interactive browser inspection was performed; frontend verification covered the production build, conversion tests, and HTTP serving.
