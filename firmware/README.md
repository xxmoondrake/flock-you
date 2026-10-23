# Detector firmware (ESP-IDF)

Passive Wi-Fi and BLE detection firmware (ESP-IDF 6.0.1), separate from the upstream Arduino firmware in the repository root, which is left unchanged.

| Environment | Board | Status |
| --- | --- | --- |
| `xiao_esp32s3` | Seeed Studio XIAO ESP32S3 | Runs. Browser dashboard (login, detection list, map, export) and USB console over its native USB port; no Wi-Fi dashboard; status LED on GPIO21; no display. GPS disabled until a breakout's wiring and supply are confirmed. |
| `heltec_v43` | Heltec WiFi LoRa 32 V4.3 (standard V4: ESP32-S3R2, 16 MB flash, native USB) | Runs. Four OLED screens with button navigation, scanning and standby modes, the browser dashboard over Wi-Fi or USB, battery monitoring and protection, recording, and the L76K GNSS receiver (NMEA on GPIO39; GPS, BeiDou, and GLONASS enabled). Pin sources are in `board_heltec_v43.cpp`. |

## Behavior

**Capture.** Wi-Fi in promiscuous receive on every 2.4 GHz channel the configured country allows (1-11 by default, 1-13 with a country such as GB or DE), highest first, 250 ms each. Flock cameras probe while stepping up through the channels about every 125 ms (upstream's observation), and their own traffic sits on whatever channel their network uses; upstream's 1/6/11 plan is an option. 5 GHz is out of reach: the ESP32-S3 radio is 2.4 GHz only. BLE: passive scanning (30 ms window every 300 ms). Nothing is transmitted while observing. Capture callbacks only pre-filter and copy into bounded queues for a matcher task; full queues drop and count, never blocking the radios.

**Matching.** Upstream flock-you's Flock signatures (pinned at upstream commit `6704f01`), reviewed on 2026-10-22 against the device families of datafx/alpr_detector: a 78-entry IEEE vendor watchlist (ALPR, body cameras, two-way radios, vehicle networking, speed radar, light bars, drones, gunshot sensors), Axon, BodyWorn, and Motorola BLE identifiers, and Flock's SoftAP SSID. Every identifier was checked against the IEEE or Bluetooth SIG registries or a published field capture; sources, labels, and the changes from upstream are in [`datasets/esp_idf_firmware_signatures.md`](../datasets/esp_idf_firmware_signatures.md). Each match is **specific** or **possible** (broad manufacturer prefixes or generic identifiers); neither label proves what a device is. Device types group matches by gear category. The console reports a match at most once per address per 5 s unless it strengthens, with fix state (`gps=current hdop=…` or `no_fix`) but no coordinates. A specific match flashes the LED three times.

**Recording.** Every match, before rate limiting, is queued to the recorder, which groups an address's sightings into an encounter. An encounter keeps its strongest rule, count, RSSI range, and three sightings (first, last, strongest), each with time, RSSI, location, and fix state. It is written on open, while changing (rate-limited), and on close, so a power cut loses about a minute at most. Boards that measure a battery can also log battery samples (`FY_BATTERY_LOG`, off by default).

**Storage.** The 4 MB `obslog` partition (0x310000) is an append-only ring of 128-byte records with sequence numbers and CRCs. Each sector is erased just before reuse, trimming the oldest records; a record torn by power loss or a failed write fails its CRC and is skipped, and records written after it in the same sector are still found at mount. Flash that cannot be read fails the mount instead of passing for an empty log. A failed encounter write stays pending and is retried; if writes keep failing and the open-encounter table fills, the oldest is given up and counted. A sector whose erase fails keeps its records, and their counts, until a retried erase succeeds; an erase that fails partway is re-read before anything is reported. Firmware uploads leave the partition intact. A boot session counter in NVS distinguishes uptimes across boots.

**GPS.** Reads RMC, GGA, GSA, GSV, and TXT from any talker. At boot the Heltec configures its L76K as Meshtastic does, without saving to the receiver: `$PCAS06,0` (version, logged as `receiver=`) and `$PCAS04,7` (GPS + BeiDou + GLONASS). Time and position validity are tracked separately; a position over 5 s old is stale and never shown as current, and dates before 2025 are treated as receiver placeholders.

**Battery (Heltec).** Measured every 5 s through the divider on GPIO1 (connected by GPIO37 only while measuring), and smoothed; with `FY_BATTERY_LOG` enabled, a sample is also stored every 60 s to characterise a battery. The percentage follows a discharge curve measured on a Heltec V4.3 with a 3000 mAh cell under normal load (about 20.5 hours from a full charge to the cutoff) and estimates the share of runtime left; the empty and full voltages stretch it to another battery or divider. Readings while charging overstate the charge. Protection: below the cutoff for 30 s, the board shows "LOW BATTERY" for 3 s, saves open encounters, and powers off. Below the start minimum it refuses to start, waking periodically without the display and starting once charging has raised the voltage.

**Heltec screens** (2 Hz):
- **Home:** battery percentage and voltage, UTC or uptime, capture state, GPS state and coordinates, match counts, last match. A specific match inverts the title row for 10 s. Without a fix, the coordinates row shows `view N sig N ant OK|OPEN|SHORT|?` (satellites in view, satellites with signal, antenna report). A passive patch antenna can read `OPEN` and still work; satellites with signal is the number that matters. Past the storage warning threshold, `STORAGE N% OFFLOAD` replaces the first match row.
- **Detections:** distinct devices per device type, this session and in stored history; possible types are marked `?`. The title shows `+` when counts are lower bounds, and `~` while the stored counts are recounted after old records left the log (the previous counts stay shown until then).
- **Data:** record counts, storage use (highlighted past the warning threshold), oldest record date. Menu: save open encounters (the notice says whether everything recorded since the previous save was written, including encounters given up while writes were failing), or erase all records behind a confirmation that starts on "Cancel". An erase also discards open encounters and the sightings queued or captured before it, and records nothing while it runs, so no erased history comes back.
- **Device:** the selected mode, what is detecting, and any dashboard. Menu: Scanning, Standby, and **Start dashboard**, a submenu offering **USB dashboard** or **WiFi dashboard**; while one runs the item reads **Stop dashboard**. During a Wi-Fi session the screen shows that detection is paused and which mode resumes afterwards, the time left, the SSID, the passkey (only while it is still the generated one), the page address, and connected clients; during a USB session, the page address.

**Heltec button** (PRG, as in Meshtastic): a short press moves to the next screen or menu item; a hold of about 0.5 s opens the menu or selects; a hold of about 4 s, then release, saves and powers off (only after 30 s of uptime). A press wakes the board.

**Modes.** Every boot starts in **Scanning**. **Standby** keeps the board on with Wi-Fi and BLE detection stopped; GPS, recording, and the battery monitor keep running. The Home screen's capture row turns inverted in standby and during a dashboard session, so it never implies detection that is not running.

**Dashboard.** A browser app served by the device over USB or Wi-Fi (below), behind a login on both:
- **Login.** Every device starts with the build-time default password (`FY_DASHBOARD_PASSWORD`, `changeme`). The first login with it opens a page that asks for a new password twice; nothing else opens until it is replaced. The new password (8-64 characters) is stored in NVS as a salted PBKDF2-HMAC-SHA256 hash (10,000 iterations); later changes need the current password and log out other browsers. Sessions are random 128-bit cookies held in RAM (30 minutes idle, 12 hours at most, cleared on reboot). After five wrong passwords in a row, each further attempt waits 5 s, doubling up to 5 minutes. Form posts are limited to 1 KB arriving within 5 s; a slower or larger one ends the connection. Pages are plain HTTP, so the login protects the history from someone who joins the link, not from someone who can read its traffic; the links themselves are WPA3 or a USB cable. HTTPS was built and dropped (2026-10-23): Android's sign-in window refuses a certificate the device signs itself, and no public authority can sign one for it.
- **Detections.** One row per address, most recently updated first, with type, match strength (specific if any of its detections was), address, advertised name, number of detections (stored encounters) and sightings, last time seen, and strongest signal. Bluetooth devices that change their address appear once per address. Search (address, name, type, vendor, or note) and a filter (specific, possible, or by your label) narrow the list; selecting a row shows its details and every detection of the address. Times are local times from GPS time or a dashboard phone's clock, or the time since that boot when there was neither.
- **Notes and ignored addresses.** Each address can carry a label (confirmed or false positive), a short note (63 printable ASCII characters), and an ignore flag; choosing the false-positive label ticks ignore, which can be unticked before saving. Ignored addresses are left off the list, map, GPX, and downloads, and raise no alerts, unless asked for; the detector still records them. The Device tab lists ignored addresses with a way to stop ignoring each. Notes are kept in NVS for up to 64 addresses (`/api/notes`) and appear in the exports.
- **Live updates.** While the page is on screen it checks the device's status every 5 s (paused while hidden) and, when new records were written, fetches only the encounters updated since its last fetch (`/api/detections?since=N`, with the next N in the `X-FY-Next-Seq` header). New detections, and detections that become specific matches, show a banner, add a count to the page title, and vibrate the phone where the browser allows it. These requests are marked as background ones: they keep neither a Wi-Fi session nor the login alive, so an unattended page still logs out after 30 minutes. After an erase or a reboot the page drops what it had loaded and loads it again, and detections whose records left the log as it wrapped disappear from the page.
- **The phone's clock.** The page sends the phone's clock when it opens, and the recorder uses it for detections without GPS time until reboot, so a XIAO without GPS still records dates and times. Positions come only from the detector's own GPS: browsers share the phone's location only with HTTPS pages.
- **Map.** A red dot per row with a position, at the detector's own position at the strongest sighting (hollow for a stale fix), on a plain grid with a scale bar. There are no map tiles: the detector stores none, and a phone on its network has no internet path to fetch them (the detector is its gateway and DNS). An optional track joins the detector's own positions at its sightings in time order (it records its position only when it detects something), breaking at gaps over 10 minutes and at reboots. Each dot is its own element so it can later be dragged to correct a position. **Open in maps app** in a row's details drops a labelled pin there (a `geo:` link, or Apple Maps on iOS).
- **Export.** CSV and JSON downloads of every stored detection event (`/export/detections.csv`, `.json`) with times in UTC, the detector's position at the first, last, and strongest sighting (with its source and HDOP), and the owner's notes; ignored addresses are left out unless `?ignored=1` (an Export tab option). Exporting deletes nothing. A download is the log as it was when it started: if records leave the log while it streams (an erase, or the ring overwriting its oldest sector), it ends incomplete rather than mixing the two, and can be repeated. A GPX download, built in the browser, has one pin per address for offline map apps such as Organic Maps and OsmAnd, which show them on their own downloaded maps. Advertised names are escaped for JSON, and CSV fields that a spreadsheet would run as formulas are prefixed with an apostrophe. Phone sign-in windows may not download files; open the address in the browser instead.
- **Device.** Board, detection state (from the detector's actual Wi-Fi and Bluetooth capture), battery, GPS, the phone's clock, storage, and uptime; the vibration setting; ignored addresses; changing the password; logging out.

The device streams the log newest sector first, one 4 KB sector at a time so the recorder is never held up, and sends each encounter's current snapshot only; the phone or computer builds the list and map. Forgotten password: erase the NVS partition (`esptool.py --chip esp32s3 erase_region 0x9000 0x6000`). That restores the default password and also replaces the Wi-Fi SSID and passkey, and the notes on addresses; records are kept.

**Wi-Fi dashboard (Heltec).** Starting it from the Device menu pauses Wi-Fi and BLE detection and turns the Wi-Fi radio into a WPA3-Personal access point (SAE only, protected management frames required, at most 2 clients) serving the dashboard at `http://192.168.4.1/`. The SSID and a temporary passkey are generated per device on first use from the hardware RNG and kept in NVS; the passkey is never logged. A session ends when stopped from the menu, after 10 minutes without a button press or dashboard request, or on reboot, and the selected mode then resumes. Ending a session also releases the access point's network interface and DHCP server; only lwIP's core thread stays, since ESP-IDF cannot shut it down once started. On recent Android versions the browser asks once for the "Nearby devices" permission before it can open a local address such as the dashboard.

The XIAO has no Wi-Fi dashboard (`FY_WIFI_DASHBOARD` off); its dashboard is USB. A board without a display that enables it starts and stops it with five quick presses of its button (each under 0.5 s, under 1 s apart) and uses a shared passkey set at build time (`FY_DASHBOARD_PASSKEY`), since there is no screen to show a generated one; anyone who knows a shared passkey can join while the dashboard is on.

**USB dashboard.** The native USB port becomes a composite device: a CDC-ACM serial port that carries the console, and a CDC-NCM network adapter (Windows 11, Linux, Android, and iOS include a driver). The device is `192.168.7.1`, and the dashboard is served at `http://192.168.7.1/`. Detection keeps running, and there is no inactivity timeout. The XIAO runs it from boot; the Heltec starts it from the Device menu, and its USB-C port returns to the USB Serial/JTAG console when it stops. Boot logs before the switch go to USB Serial/JTAG; panic output after it is not visible over USB.

**Captive portal (default, `FY_DASHBOARD_CAPTIVE_PORTAL`).** Both dashboard networks offer the device as gateway and DNS server, a DNS server answers every name with the device's address, and everything except the dashboard redirects to it, including the connectivity checks Android, iOS, and Windows make (which do not count as Wi-Fi session activity). Android then opens the dashboard in its "sign in to network" window with mobile data on, and `http://dash.fy/` (`FY_DASHBOARD_HOST`) reaches it wherever name lookups go to the device (a PC that asks its own DNS servers first may not resolve it; the address always works); iOS reaches it in the browser over USB, and through the portal over Wi-Fi. A PC on the USB link also gets a default route and DNS server there, normally outranked by its other connections; turn the option off for development use if that matters.

**Status LED.** Solid while booting, one short blink every 3 s while scanning, one every 10 s in standby, and a double blink every 3 s while the Wi-Fi dashboard runs. Default on the XIAO; the Heltec uses its LED only for match flashes.

**Status lines** every 30-60 s: capture counts, queue drops, truncations, free heap, GPS health, battery, storage use.

### Configuration (Kconfig)

| Option | Default | Meaning |
| --- | --- | --- |
| `FY_ENCOUNTER_ABSENCE_S` | 60 | Seconds of silence that close an encounter |
| `FY_ENCOUNTER_CHECKPOINT_S` | 60 | Minimum seconds between rewrites of a changing encounter |
| `FY_BATTERY_LOG` | off | Store battery samples (for characterising a battery) |
| `FY_BATTERY_LOG_INTERVAL_S` | 60 | Battery sample interval (s), when logging |
| `FY_STORAGE_WARN_PERCENT` | 90 | Storage use that triggers the offload warning |
| `FY_BATTERY_EMPTY_MV`, `FY_BATTERY_FULL_MV` | 3200, 4270 | Voltages shown as 0% and 100% |
| `FY_BATTERY_CUTOFF_MV` | 3200 | Power off after 30 s below this |
| `FY_BATTERY_BOOT_MIN_MV` | 3350 | Refuse to start below this |
| `FY_BATTERY_RECHECK_S` | 600 | Wake interval while off for low battery |
| `FY_STATUS_LED_PATTERNS` | XIAO only | Show the operating mode on the LED |
| `FY_GPS_LOG_RAW_NMEA` | off | Echo raw NMEA to the console (`heltec_v43_debug_nmea`) |
| `FY_SERIAL_JSON` | on | Print upstream dashboard JSON for each reported match |
| `FY_WIFI_COUNTRY` | `01` (world safe: channels 1-11) | Regulatory domain: sets the swept channels and the dashboard's transmit rules |
| `FY_WIFI_CHANNELS_UPSTREAM` | off | Sweep only channels 11, 6, and 1 |
| `FY_WIFI_DWELL_MS` | 250 | Time on each channel |
| `FY_SIG_MOTOROLA`, `FY_SIG_SIERRA_WIRELESS` | on | Match these broad vendors (labeled possible) |
| `FY_SIG_CRADLEPOINT` | off | Match Cradlepoint vehicle routers (noisy in the reference's field tests) |
| `FY_SIG_RADIO_OUIS_ANY_FRAME` | off | Match the Flock radio-module OUIs (Lite-On and others) on any frame, as upstream does, not only in wildcard probes |
| `FY_DASHBOARD_SSID_PREFIX` | `FY-` | Access point name prefix; four random characters follow |
| `FY_DASHBOARD_CHANNEL` | 6 | Access point channel |
| `FY_DASHBOARD_IDLE_S` | 600 | End a dashboard session after this long without use |
| `FY_DASHBOARD_PASSWORD` | `changeme` | Default dashboard password, replaced at first login |
| `FY_DASHBOARD_PASSKEY` | empty | Shared Wi-Fi passkey for boards without a display; empty generates one per device |
| `FY_USB_DASHBOARD` | on (both boards enable TinyUSB NCM and CDC) | USB console and dashboard over USB networking |
| `FY_USB_DASHBOARD_AT_BOOT` | XIAO only | Start the USB dashboard at boot instead of from the Device menu |
| `FY_WIFI_DASHBOARD` | Heltec only | Offer the Wi-Fi dashboard |
| `FY_DASHBOARD_CAPTIVE_PORTAL` | on | Present the dashboards as a captive portal |
| `FY_DASHBOARD_HOST` | `dash.fy` | A name the portal's DNS answers for the dashboard (`http://dash.fy/`) |

## Planned

- Field validation of the reviewed signatures and a review of the legacy flocker rules. 5 GHz coverage would need a different chip (an ESP32-C5, for example).
- Dashboard: clearing records, changing the Wi-Fi passkey, password recovery that erases history, and dragging a map dot to correct a detection's position.
- GPS on the XIAO via an external NMEA receiver.
- Optional position estimate for a detected transmitter from several located sightings and their signal strengths.
- Battery cutoff and start thresholds revisited against the measured curve, and the divider checked with a multimeter.

## Reading stored records

The dashboard's Export tab downloads every detection as CSV or JSON. Without the dashboard, dump the partition over USB and decode it. A full 4 MB read takes about 6 minutes over native USB; if the log has not wrapped, the first `records × 128` bytes (rounded up to 4 KB; the record count is in the recorder status line) are enough.

```bash
esptool.py --chip esp32s3 --port <PORT> read_flash 0x310000 0x400000 obslog.bin
python tools/read_obslog.py obslog.bin --battery-csv battery.csv --encounters-csv encounters.csv
```

Encounter output contains positions; treat it as sensitive location history.

## Upstream dashboard

Each reported match is also printed as one JSON line in upstream's format, so the Flask dashboard in [`api/`](../api/) can read either board over USB serial. The dashboard ignores the non-JSON log lines. Lines carry no location; the dashboard adds the host's GPS. They also add this firmware's `rule` and `strength`. Identifier sources are in [`datasets/esp_idf_firmware_signatures.md`](../datasets/esp_idf_firmware_signatures.md).

```json
{"event":"detection","detection_method":"wifi_wildcard_probe_ie_sig","detection_tier":4,"protocol":"wifi_2_4ghz","mac_address":"b4:1e:52:01:02:03","oui":"b4:1e:52","device_name":"","rssi":-52,"channel":6,"frequency":2437,"ssid":"","rule":"wifi_probe_ie_sig","strength":"specific"}
```

| Rules | `detection_method` | Tier |
| --- | --- | --- |
| `wifi_probe_ie_sig` | `wifi_wildcard_probe_ie_sig` | 4 |
| `wifi_wildcard_probe` | `wifi_wildcard_probe` | 3 |
| `wifi_oui_addr2` | `wifi_oui_addr2` | 2 |
| `wifi_oui_addr1`, `wifi_oui_addr3` | same as the rule | 1 |
| `wifi_ssid_flock` | `wifi_ssid` (the SSID in `ssid`) | 3 |
| BLE name rules | `ble_name` | 2 |
| BLE company ID rules | `ble_mfg` | 2 |
| BLE service rules and the Axon tag | `ble_gatt_svc` (channel 255, frequency 0) | 2 |
| `ble_oui` | `ble_oui` | 1 |

`strength` is the match's own: a watchlist OUI reports its vendor's label.

The firmware does not read dashboard commands. It has no buzzer for the beep controls, and it does not dump stored records over serial (`dump_session`), because export belongs to the authenticated dashboard (or esptool, as above).

## Layout

The host-tested parts are pure C++ with no ESP-IDF dependencies; `test/` holds their unit tests (set `FY_RENDER_DIR` to write sample screens as PBM images).

| Path | Contents | Host-tested |
| --- | --- | --- |
| `components/fy_signatures` | Signature data and matching, upstream dashboard JSON | All |
| `components/fy_detector` | Wi-Fi and BLE capture, queues, matcher, reporting | |
| `components/fy_board` | Board profiles (`CONFIG_FY_BOARD_*`): pins, power, LED, SSD1306 transport, GNSS UART | |
| `components/fy_gps` | NMEA parser, receive-only UART driver | Parser |
| `components/fy_display` | 128x64 canvas, 5x7 font | All |
| `components/fy_ui` | Screen layouts and navigation, display and button task | Layouts, navigation |
| `components/fy_status_led` | LED patterns, LED task | Patterns |
| `components/fy_device` | Mode and dashboard-session logic, device task applying it to the radios and LED | Mode logic |
| `components/fy_dashboard` | Login, pages, exports, notes, captive portal, the web app (`web/`), WPA3 access point and HTTP server | Login, pages, exports, notes, portal |
| `components/fy_usb` | Composite USB device (CDC-ACM console, CDC-NCM network), bootloader reboot on a 1200-baud touch | |
| `src/idf_component.yml`, `dependencies.lock` | `espressif/esp_tinyusb` pinned to 2.4.0 (with `espressif/tinyusb` 0.21.0~2), fetched from the ESP Component Registry on first build | |
| `components/fy_battery` | Voltage-to-percent estimate, measurement driver | Estimate |
| `components/fy_store` | Record format, power-loss-tolerant ring log, encounter grouping, reading encounters back | All |
| `components/fy_recorder` | Writes encounters (and optional battery samples) to `obslog` | |
| `tools/read_obslog.py` | Decodes an `obslog` dump to CSV | |
| `src/main.cpp` | Startup | |
| `sdkconfig.defaults*` | Shared and per-board configuration | |
| `boards/` | PlatformIO board definition for the Heltec V4.3 | |

## Build and flash

Requires PlatformIO Core; the pinned platform downloads ESP-IDF 6.0.1 and its toolchain on first build. The repository path must not contain spaces: ESP-IDF's `-fmacro-prefix-map` argument breaks, and directory junctions don't help because the path is resolved. Each environment's resolved configuration is kept in a generated `sdkconfig.<env>`, which wins over `sdkconfig.defaults*` and Kconfig defaults; after changing either, delete that file (and `.pio/build/<env>/CMakeCache.txt`) so it is regenerated. The web app's files are listed twice: as `EMBED_FILES` in the component's CMakeLists and as `board_build.embed_files` in `platformio.ini`, which PlatformIO needs as well.

```bash
pio run -e xiao_esp32s3                    # or heltec_v43
pio run -e xiao_esp32s3 -t upload -t monitor
pio test -e native                         # needs a host C++ compiler on PATH (e.g. MinGW-w64 GCC on Windows)
```

`heltec_v43_debug_nmea` echoes every raw NMEA sentence, including positions; flash the normal environment afterwards.

Flashing while a USB dashboard runs (always on the XIAO): the board's serial port is then the USB console, a different port from the bootloader's. `pio run -e <env> -t upload --upload-port <console port>` opens it at 1200 baud, which reboots the board into the serial bootloader; PlatformIO then flashes through the bootloader's port. If that fails, hold BOOT (PRG on the Heltec), tap RESET (RST), release, and flash to the port that appears.

Before flashing:
- Flashing replaces the board's existing firmware. Back up first, and restore by writing the backup at offset 0:
  `esptool.py --chip esp32s3 read_flash 0 ALL backup.bin` / `esptool.py --chip esp32s3 write_flash 0 backup.bin`
- Do not connect or power a GPS breakout on the XIAO until its receiver, supply voltage, and UART levels are confirmed.

## Signature review notes

Changes from upstream's set in the 2026-10-22 review (details in the datasets note linked above):
- The Flock radio-module OUIs (22 Lite-On blocks plus Silicon Labs, Espressif, Samsung, USI, and the Qualcomm Atheros default) also ship in laptops, phones, and IoT gear, so they now gate only the wildcard-probe rules; `FY_SIG_RADIO_OUIS_ANY_FRAME` restores any-frame matching. Espressif blocks mean an ESP32 (another detector, a test transmitter) can still match there.
- Flock Safety's own block `b4:1e:52` is specific; `b8:35:32` is gone (not an IEEE assignment).
- The bare 10-digit BLE name is retired (it matched a phone); Raven is limited to its five advertised services, read in 16-bit and 128-bit form.
- "DfuTarg", the Nordic DFU service, company ID `0x09C8`, `FS-` names, and names starting `Flock` stay or come in as possible: they are shared by unrelated products.

## License

MIT. Signature data and matching logic are ported from upstream flock-you (`main.cpp`), Copyright (c) 2026 colonelpanichacks; see `LICENSE` in the repository root.
