# Limier

![Limier](images/limier.png)

Limier is passive Wi-Fi and Bluetooth LE detection firmware for ESP32-S3 boards. It listens for Flock cameras and other surveillance and law-enforcement gear and records what it hears with the time and GPS position. Results appear on the board's screen and in a browser dashboard served by the device itself. It transmits nothing while detecting.

Limier is built on ESP-IDF 6.0.1. It is separate from the upstream Arduino firmware in the repository root.

## Boards

| Board | Build environment | Features |
| --- | --- | --- |
| Heltec WiFi LoRa 32 V4.3 | `heltec_v43` | OLED screen and button, dashboard over Wi-Fi or USB, GNSS, battery monitoring and protection |
| Heltec WiFi LoRa 32 V4 R8 | `heltec_v4_r8` | Same as the V4.3 |
| Seeed Studio XIAO ESP32S3 | `xiao_esp32s3` | Dashboard over USB, status LED |

- Never flash the `heltec_v43` build onto an R8. It drives pins that the R8 uses for its PSRAM.
- The XIAO has no GPS support yet. Don't wire a receiver to it.
- For sound alerts on any board, add a passive piezo buzzer between GPIO3 (D2 on the XIAO) and GND.

## Build

Building requires PlatformIO Core. The first build downloads ESP-IDF and its toolchain, so it takes several minutes.

```bash
cd firmware
pio run -e heltec_v43
```

Use `heltec_v4_r8` or `xiao_esp32s3` for the other boards.

- **No spaces in the path.** The repository's path must not contain spaces: ESP-IDF's build breaks on them, and a directory junction does not help.
- **Generated settings file.** Settings come from `sdkconfig.defaults` and `sdkconfig.defaults.<env>`. They are resolved into a generated `sdkconfig.<env>`, which takes precedence over both.
- **Changing options.** Run `pio run -e <env> -t menuconfig`, or edit a defaults file and then delete `sdkconfig.<env>` and `.pio/build/<env>/CMakeCache.txt` so they are regenerated. The options are listed under [Configuration](#configuration).
- **Unit tests.** Run them on the host with `pio test -e native`. This needs a C++ compiler on PATH, such as MinGW-w64 GCC on Windows.

## Flash

Flashing replaces the board's current firmware. To keep a copy of it, back it up first:

```bash
esptool.py --chip esp32s3 read_flash 0 ALL backup.bin
```

To restore the backup later, use `esptool.py --chip esp32s3 write_flash 0 backup.bin`.

Connect the board over USB-C, then flash it and open the console:

```bash
pio run -e heltec_v43 -t upload -t monitor
```

- **When the USB dashboard is running** (always, on the XIAO), the board's serial port is its USB console. PlatformIO restarts the board into its bootloader through that port, then flashes on the port that appears. If it picks the wrong port, add `--upload-port <console port>`.
- **If flashing fails,** hold BOOT (PRG on the Heltec), tap RESET (RST), and release. Then flash to the port that appears.
- **Data survives reflashing:** records, settings, notes, and the dashboard password are all kept.
- **GNSS troubleshooting:** `heltec_v43_debug_nmea` also echoes the raw GNSS sentences to the console, positions included. Flash the normal build afterwards.

## First start

1. Power the board on. Detection starts at once.
   - The Heltec shows the Limier image, then its name and versions, then the Home screen.
   - A fitted buzzer plays a start-up tune.
2. Open the dashboard:
   - **USB, any board:** connect the board to a computer or phone. The XIAO starts its USB dashboard at boot. On a Heltec, choose Device → Start dashboard → USB dashboard. Then open `http://192.168.7.1/`.
   - **Wi-Fi, Heltec only:** choose Device → Start dashboard → WiFi dashboard, then join the network shown on the screen with its passkey. A phone opens the dashboard in its sign-in window. Otherwise, open `http://dash.fy/`.
3. Log in with `changeme`, then set a new password (8-64 characters). The one password works for both dashboards.

## Heltec screen and button

The PRG button works like this:
- **Short press:** next screen or menu item.
- **Hold about 0.5 s:** open the menu, or select.
- **Hold about 4 s, then release:** save and power off. This works once the board has been on for 30 s. A press turns the board back on.

The screens:
- **Home:** battery, time, detection state, GPS state and position, match counts, and the last match.
  - A specific match inverts the title row for 10 s.
  - Without a GPS fix, the position row shows the satellites in view, the satellites with signal, and the antenna state. A passive antenna can read `OPEN` and still work.
  - `STORAGE N% OFFLOAD` means the log is nearly full.
- **Detections:** devices of each type, this session and in stored history.
  - `?` marks a possible type.
  - `+` means the counts are lower bounds.
  - `~` means the stored counts are being recounted.
- **Data:** record counts, storage use, and the oldest record.
  - The menu saves open detections, or erases all records. The erase confirmation starts on Cancel.
- **Device:** the mode, what is detecting, and any dashboard session.
  - The menu has Scanning, Standby, and Start dashboard (USB or WiFi), which reads Stop dashboard while a session runs.
  - It also has Alerts: the Alerts, Buzzer, and Screen flash switches, and Test alert.
  - During a Wi-Fi session this screen shows the network name and passkey, the address, the connected clients, and the time left.

## Modes

Every boot starts in **Scanning**.

**Standby** stops Wi-Fi and Bluetooth detection. GPS, recording, and the battery monitor keep running.

The two dashboards treat detection differently:
- The Wi-Fi dashboard pauses detection while it runs. When the session ends, the board returns to the selected mode.
- The USB dashboard does not pause detection.

The Home screen's detection row is inverted whenever detection is not running.

## Dashboard

### Connecting

- **USB.** The board appears as a network adapter, with drivers built into Windows 11, Linux, Android, and iOS, and as a serial console. The dashboard is at `http://192.168.7.1/`. On the Heltec, the USB port returns to its normal console when the session stops.
- **Wi-Fi (Heltec).** The board runs a WPA3 access point named `FY-` and four characters, with a passkey generated on first use. It allows two clients at most.
  - The session ends when stopped from the menu, after 10 minutes without a button press or page use, or on reboot.
  - Recent Android versions ask once for the "Nearby devices" permission before the browser can open the dashboard.
- **Sign-in windows.** Both links answer every name with the device, so a phone opens the dashboard in its sign-in window, and `http://dash.fy/` reaches it.
  - A computer that asks its own DNS servers first may not resolve `dash.fy`. The numeric address always works.
  - Sign-in windows may not download files. To export, open the address in a browser.
- **Installing.** "Add to Home Screen" on a phone installs the dashboard as Limier.

### Login

- **First login.** Logging in with the default password leads straight to a page that asks for a new one.
- **Changing the password.** This requires the current password and logs out other browsers.
- **Sessions.** A login lasts 30 minutes idle and 12 hours at most, and ends on reboot.
- **Wrong passwords.** After five in a row, each further attempt waits longer, up to 5 minutes.
- **Plain HTTP.** Pages are not encrypted. The login protects the detection history from someone who joins the link, not from someone reading its traffic. The link itself is WPA3 or a USB cable.
- **Forgotten password.** Run `esptool.py --chip esp32s3 erase_region 0x9000 0x6000` to restore `changeme`.
  - This also resets the Wi-Fi network name and passkey, the alert settings, and the address notes.
  - Records are kept.

### Tabs

- **Detections.** One row per address, most recently updated first.
  - Each row shows the type, match strength, address, advertised name, detections and sightings, last seen, and strongest signal.
  - Search by address, name, type, vendor, or note, and filter by strength or label.
  - A row's details list every detection of that address and hold its label (confirmed or false positive), a short note, and an ignore flag. Notes are kept for up to 64 addresses.
  - Ignored addresses are still recorded, but raise no alerts. They are hidden from the list and map unless **Show ignored** is ticked.
  - While the page is open it checks for new records every 5 s. New detections show a banner, add a count to the page title, and vibrate the phone where the browser allows it.
- **Map.** A dot for each address, at the detector's position at its strongest sighting. A hollow dot means the fix was stale.
  - The map is a plain grid with no map tiles: the device stores none, and a phone connected to it has no internet.
  - An optional track joins the detector's positions in time order.
  - **Open in maps app,** in a row's details, drops a pin in the phone's maps app.
- **Export.** Every stored detection, in UTC with the detector's positions and the address notes.
  - CSV, JSON, and GPX (one pin per address, for offline map apps such as Organic Maps and OsmAnd).
  - Files are named `limier-detections-YYYY-MM-DD`.
  - Ignored addresses are left out unless the tab's option to include them is ticked.
  - Exporting deletes nothing.
- **Device.**
  - **Settings:** Scanning or Standby, alerts, saving open detections, and erasing all records.
  - **Status:** board, firmware, detection, battery, GPS, storage, and uptime.
  - **Other:** vibration, ignored addresses, the password, and log out.

Times come from GPS, or from the phone's clock, which the page sends to the device when it opens. This means a XIAO without GPS still records dates. Positions come only from the detector's own GPS.

## Alerts

With a passive buzzer fitted on GPIO3, Limier plays upstream flock-you's sounds:
- **Start-up:** six notes at boot.
- **New device:** an address that is new, back after 30 s of silence, or seen at a higher confidence tier plays its tier's sound.
  - Tiers 4 and 3 play two rising notes.
  - Tiers 2 to 0 play a single blip.
- **Heartbeat:** two beeps every 10 s while anything stays in range.

On the Heltec, a new specific match also flashes the Limier image on the screen, regular and inverted. Each address flashes the screen at most once a minute.

The settings are kept across restarts, and all start on:
- **Alerts:** the master switch.
- **Buzzer** and **Screen flash.**
- **A mute for each tier.**

Alert settings can be changed from the dashboard's Device tab. On the Heltec, all except the tier mutes are also under Device → Alerts. **Test alert** plays the alert once. Turning alerts off doesn't stop detection or recording.

## Status LED

- **XIAO:** solid while booting, then a short blink every 3 s while scanning and every 10 s in standby.
- **Both boards:** a specific match flashes the LED three times.

## Battery (Heltec)

The battery is shown as the share of runtime left. It follows a discharge curve measured on a 3000 mAh cell, which lasts about 20 hours. Readings while charging run high.

- **Low-battery cutoff:** below 3.2 V for 30 s, the board saves its records and powers off.
- **Start minimum:** below 3.35 V, the board won't start. It checks again every 10 minutes and starts once charging has raised the voltage.

## What Limier detects

- **Wi-Fi:** every 2.4 GHz channel the configured country allows, 250 ms each. That is channels 1-11 by default; setting `FY_WIFI_COUNTRY` to the local country code adds 12 and 13 where they are allowed. The ESP32-S3 can't receive 5 GHz.
- **Bluetooth LE:** passive scanning.
- **Signatures:** upstream flock-you's Flock signatures, plus a watchlist of makers of the following:
  - ALPR cameras, body cameras, and gunshot sensors;
  - two-way radios, vehicle networks, speed radar, and light bars;
  - drones.

  The full list and its sources are in [`datasets/esp_idf_firmware_signatures.md`](../datasets/esp_idf_firmware_signatures.md).
- **Specific and possible.** Each match is labeled one or the other. Neither label proves what a device is.
  - **Specific:** an identifier that points to the gear.
  - **Possible:** a broad manufacturer prefix or a generic identifier.

## Records

Limier groups each address's sightings into an encounter. Each encounter keeps the time, signal strength, and GPS position of its first, last, and strongest sighting.

Encounters are stored in a 4 MB log in flash:
- A power cut loses about a minute at most.
- When the log is full, the oldest records are overwritten.
- Records survive reboots and firmware updates.

Records and exports are location history, so keep them private.

The log can also be read without the dashboard. Dump it over USB, which takes about 6 minutes, then decode it:

```bash
esptool.py --chip esp32s3 --port <PORT> read_flash 0x310000 0x400000 obslog.bin
python tools/read_obslog.py obslog.bin --encounters-csv encounters.csv
```

## Upstream's Flask dashboard

Each match is also printed to the console as one JSON line in upstream's format, so the Flask dashboard in [`api/`](../api/) can read Limier over USB serial.
- **Location:** the lines carry none; the Flask dashboard adds the host computer's GPS.
- **Extra fields:** each line adds Limier's `rule` and `strength`.
- **Commands:** Limier ignores the Flask dashboard's commands. Use Limier's own dashboard instead.

| Rules | `detection_method` | Tier |
| --- | --- | --- |
| `wifi_probe_ie_sig` | `wifi_wildcard_probe_ie_sig` | 4 |
| `wifi_wildcard_probe` | `wifi_wildcard_probe` | 3 |
| `wifi_ssid_flock` | `wifi_ssid` | 3 |
| `wifi_oui_addr2` | `wifi_oui_addr2` | 2 |
| BLE name, company ID, and service rules | `ble_name`, `ble_mfg`, `ble_gatt_svc` | 2 |
| `wifi_oui_addr1`, `wifi_oui_addr3`, `ble_oui` | same as the rule | 1 |

## Configuration

These are Kconfig options. Set them as described under [Build](#build).

| Option | Default | Meaning |
| --- | --- | --- |
| `FY_WIFI_COUNTRY` | `01` (channels 1-11) | Regulatory domain: sets the swept channels and the access point's transmit rules |
| `FY_WIFI_CHANNELS_UPSTREAM` | off | Sweep only channels 11, 6, and 1, as upstream does |
| `FY_WIFI_DWELL_MS` | 250 | Time on each channel |
| `FY_SIG_MOTOROLA`, `FY_SIG_SIERRA_WIRELESS` | on | Match these broad vendors (labeled possible) |
| `FY_SIG_CRADLEPOINT` | off | Match Cradlepoint vehicle routers (mostly school buses and traffic cameras) |
| `FY_SIG_RADIO_OUIS_ANY_FRAME` | off | Match the Flock radio-module OUIs on any frame, as upstream does, not only in wildcard probes |
| `FY_SERIAL_JSON` | on | Print upstream dashboard JSON for each match |
| `FY_ENCOUNTER_ABSENCE_S` | 60 | Seconds of silence that close an encounter |
| `FY_ENCOUNTER_CHECKPOINT_S` | 60 | Minimum seconds between rewrites of an open encounter |
| `FY_STORAGE_WARN_PERCENT` | 90 | Storage use that shows the offload warning |
| `FY_BATTERY_EMPTY_MV`, `FY_BATTERY_FULL_MV` | 3200, 4270 | Voltages shown as 0% and 100%, to fit another battery |
| `FY_BATTERY_CUTOFF_MV` | 3200 | Power off after 30 s below this |
| `FY_BATTERY_BOOT_MIN_MV` | 3350 | Refuse to start below this |
| `FY_BATTERY_RECHECK_S` | 600 | Check interval while off for low battery |
| `FY_BUZZER_GPIO` | 3 | GPIO of the passive buzzer (-1: none) |
| `FY_STATUS_LED_PATTERNS` | XIAO only | Show the mode on the LED |
| `FY_USB_DASHBOARD` | on | Offer the dashboard and console over USB |
| `FY_USB_DASHBOARD_AT_BOOT` | XIAO only | Start the USB dashboard at boot instead of from the Device menu |
| `FY_WIFI_DASHBOARD` | Heltec only | Offer the Wi-Fi dashboard. A board without a screen starts and stops it with five quick button presses |
| `FY_DASHBOARD_SSID_PREFIX` | `FY-` | Access point name prefix; four random characters follow |
| `FY_DASHBOARD_PASSKEY` | empty | Shared Wi-Fi passkey for boards without a screen; empty generates one per device |
| `FY_DASHBOARD_CHANNEL` | 6 | Access point channel |
| `FY_DASHBOARD_IDLE_S` | 600 | End a dashboard session after this long without use |
| `FY_DASHBOARD_PASSWORD` | `changeme` | Default dashboard password, replaced at first login |
| `FY_DASHBOARD_CAPTIVE_PORTAL` | on | Open the dashboard in phones' sign-in windows. A computer on the USB link also gets a default route and DNS server through the device, normally outranked by its other connections |
| `FY_DASHBOARD_HOST` | `dash.fy` | The dashboard's name on the device's network |

## Planned

- **Bark (Heltec):** share new detections on a Meshtastic channel over the onboard LoRa radio.
  - The channel is set in the dashboard.
  - Off by default, with a receive-only option.
  - Messages include a location only when there is a fresh GPS fix.
- **Bloodhound (Heltec):** find the identifiers of a visible device that Limier doesn't recognize.
  - Sample near it and away from it using the button.
  - Then review the ranked candidates in the dashboard.
- **Dashboard:** changing the Wi-Fi passkey, resetting a forgotten password by erasing history, and dragging a map dot to correct a position.
- **XIAO:** GPS through an external receiver.
- **Position estimates:** an estimated transmitter position from several located sightings.

## Source layout

| Path | Contents |
| --- | --- |
| `src/main.cpp` | Start-up |
| `components/fy_board` | Board profiles: pins, power, LED, display, GNSS UART, buzzer |
| `components/fy_detector` | Wi-Fi and BLE capture and matching |
| `components/fy_signatures` | Signature data and matching rules |
| `components/fy_recorder`, `fy_store` | Encounters and the flash log |
| `components/fy_gps` | NMEA parser and receiver driver |
| `components/fy_battery` | Battery measurement and charge estimate |
| `components/fy_display`, `fy_ui` | Screen drawing, layouts, button navigation, start-up screens |
| `components/fy_alerts` | Alert settings and sounds |
| `components/fy_status_led` | LED patterns |
| `components/fy_device` | Modes and dashboard sessions |
| `components/fy_dashboard` | Dashboard server, login, exports, settings, captive portal, and the web app (`web/`) |
| `components/fy_usb` | USB console and network adapter |
| `test/` | Host unit tests (`pio test -e native`) |
| `tools/` | `read_obslog.py` (log decoder) and `frame_from_png.py` (start-up image converter) |
| `boards/` | Board definitions for the Heltec V4.3 and V4 R8 |

## License

MIT. Signature data and matching logic are ported from upstream flock-you (`main.cpp`), Copyright (c) 2026 colonelpanichacks; see `LICENSE` in the repository root.
