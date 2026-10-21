# Detector firmware (ESP-IDF)

Passive Wi-Fi and BLE detection firmware (ESP-IDF 6.0.1), separate from the upstream Arduino firmware in the repository root, which is left unchanged.

| Environment | Board | Status |
| --- | --- | --- |
| `xiao_esp32s3` | Seeed Studio XIAO ESP32S3 | Runs. Status LED on GPIO21; no display. GPS disabled until a breakout's wiring and supply are confirmed. |
| `heltec_v43` | Heltec WiFi LoRa 32 V4.3 (standard V4: ESP32-S3R2, 16 MB flash, native USB) | Runs. OLED screens, button navigation, battery monitoring and protection, recording, and the L76K GNSS receiver (NMEA on GPIO39; GPS, BeiDou, and GLONASS enabled). Pin sources are in `board_heltec_v43.cpp`. |

## Behavior

**Capture.** Wi-Fi in promiscuous receive on channels 11/6/1 (250 ms dwell); passive BLE scanning (30 ms window every 300 ms). Nothing is transmitted while observing. Capture callbacks only pre-filter and copy into bounded queues for a matcher task; full queues drop and count, never blocking the radios.

**Matching.** Upstream flock-you signatures, pinned at upstream commit `6704f01`. Each rule is **specific** or **possible** (broad manufacturer prefixes or generic identifiers); neither label proves what a device is. The console reports a match at most once per address per 5 s unless it strengthens, with fix state (`gps=current hdop=…` or `no_fix`) but no coordinates. A specific match flashes the LED three times.

**Recording.** Every match, before rate limiting, is queued to the recorder, which groups an address's sightings into an encounter. An encounter keeps its strongest rule, count, RSSI range, and three sightings (first, last, strongest), each with time, RSSI, location, and fix state. It is written on open, while changing (rate-limited), and on close, so a power cut loses about a minute at most. Boards that measure a battery also log battery samples.

**Storage.** The 4 MB `obslog` partition (0x310000) is an append-only ring of 128-byte records with sequence numbers and CRCs. Each sector is erased just before reuse, trimming the oldest records; a record torn by power loss fails its CRC and is skipped. Firmware uploads leave the partition intact. A boot session counter in NVS distinguishes uptimes across boots.

**GPS.** Reads RMC, GGA, GSA, GSV, and TXT from any talker. At boot the Heltec configures its L76K as Meshtastic does, without saving to the receiver: `$PCAS06,0` (version, logged as `receiver=`) and `$PCAS04,7` (GPS + BeiDou + GLONASS). Time and position validity are tracked separately; a position over 5 s old is stale and never shown as current, and dates before 2025 are treated as receiver placeholders.

**Battery (Heltec).** Measured every 5 s through the divider on GPIO1 (connected by GPIO37 only while measuring), smoothed, and logged every 60 s. The percentage stretches a typical Li-ion curve between the empty and full voltages; tune both to the battery. Readings while charging overstate the charge. Protection: below the cutoff for 30 s, the board shows "LOW BATTERY" for 3 s, saves open encounters, and powers off. Below the start minimum it refuses to start, waking periodically without the display and starting once charging has raised the voltage.

**Heltec screens** (2 Hz):
- **Home:** battery percentage and voltage, UTC or uptime, capture state, GPS state and coordinates, match counts, last match. A specific match inverts the title row for 10 s. Without a fix, the coordinates row shows `view N sig N ant OK|OPEN|SHORT|?` (satellites in view, satellites with signal, antenna report). A passive patch antenna can read `OPEN` and still work; satellites with signal is the number that matters. Past the storage warning threshold, `STORAGE N% OFFLOAD` replaces the first match row.
- **Detections:** distinct devices per device type, this session and in stored history; possible types are marked `?`.
- **Data:** record counts, storage use (highlighted past the warning threshold), oldest record date. Menu: save open encounters, or erase all records behind a confirmation that starts on "Cancel".

**Heltec button** (PRG, as in Meshtastic): a short press moves to the next screen or menu item; a hold of about 0.5 s opens the menu or selects; a hold of about 4 s, then release, saves and powers off (only after 30 s of uptime). A press wakes the board.

**Status LED.** Solid while booting, then one short blink every 3 s while scanning. Default on the XIAO; the Heltec uses its LED only for match flashes.

**Status lines** every 30-60 s: capture counts, queue drops, truncations, free heap, GPS health, battery, storage use.

### Configuration (Kconfig)

| Option | Default | Meaning |
| --- | --- | --- |
| `FY_ENCOUNTER_ABSENCE_S` | 60 | Seconds of silence that close an encounter |
| `FY_ENCOUNTER_CHECKPOINT_S` | 60 | Minimum seconds between rewrites of a changing encounter |
| `FY_BATTERY_LOG_INTERVAL_S` | 60 | Battery sample interval (s) |
| `FY_STORAGE_WARN_PERCENT` | 90 | Storage use that triggers the offload warning |
| `FY_BATTERY_EMPTY_MV`, `FY_BATTERY_FULL_MV` | 3300, 4200 | Voltages shown as 0% and 100% |
| `FY_BATTERY_CUTOFF_MV` | 3200 | Power off after 30 s below this |
| `FY_BATTERY_BOOT_MIN_MV` | 3350 | Refuse to start below this |
| `FY_BATTERY_RECHECK_S` | 600 | Wake interval while off for low battery |
| `FY_STATUS_LED_PATTERNS` | XIAO only | Show the operating mode on the LED |
| `FY_GPS_LOG_RAW_NMEA` | off | Echo raw NMEA to the console (`heltec_v43_debug_nmea`) |
| `FY_SERIAL_JSON` | on | Print upstream dashboard JSON for each reported match |

## Planned

- Signature review against the device families in datafx/alpr_detector, taking identifiers only, each checked against a primary source (IEEE or Bluetooth SIG registry, or field captures).
- Browser dashboard (USB networking on the XIAO; a temporary Wi-Fi access point started from the Heltec menu) with per-device authentication, CSV and JSON export, and clearing.
- Temporary Wi-Fi sessions: Wi-Fi detection paused, shown on the OLED and LED (double blink every 3 s), ending after 10 minutes of inactivity.
- GPS on the XIAO via an external NMEA receiver; the phone's location as an optional dashboard source.
- Optional position estimate for a detected transmitter from several located sightings and their signal strengths.
- Battery thresholds calibrated from recorded charge and discharge data.

## Reading stored records

With USB access, dump the partition and decode it. A full 4 MB read takes about 6 minutes over native USB; if the log has not wrapped, the first `records × 128` bytes (rounded up to 4 KB; the record count is in the recorder status line) are enough.

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
| BLE name rules | `ble_name` | 2 |
| `ble_mfg_09c8` | `ble_mfg` | 2 |
| BLE service rules | `ble_gatt_svc` (channel 255, frequency 0) | 2 |

The firmware does not read dashboard commands. It has no buzzer for the beep controls, and it does not dump stored records over serial (`dump_session`), because export is planned for the authenticated dashboard; read records with esptool as above.

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
| `components/fy_battery` | Voltage-to-percent estimate, measurement driver | Estimate |
| `components/fy_store` | Record format, power-loss-tolerant ring log, encounter grouping | All |
| `components/fy_recorder` | Writes encounters and battery samples to `obslog` | |
| `tools/read_obslog.py` | Decodes an `obslog` dump to CSV | |
| `src/main.cpp` | Startup | |
| `sdkconfig.defaults*` | Shared and per-board configuration | |
| `boards/` | PlatformIO board definition for the Heltec V4.3 | |

## Build and flash

Requires PlatformIO Core; the pinned platform downloads ESP-IDF 6.0.1 and its toolchain on first build. The repository path must not contain spaces: ESP-IDF's `-fmacro-prefix-map` argument breaks, and directory junctions don't help because the path is resolved.

```bash
pio run -e xiao_esp32s3                    # or heltec_v43
pio run -e xiao_esp32s3 -t upload -t monitor
pio test -e native                         # needs a host C++ compiler on PATH (e.g. MinGW-w64 GCC on Windows)
```

`heltec_v43_debug_nmea` echoes every raw NMEA sentence, including positions; flash the normal environment afterwards.

Before flashing:
- Flashing replaces the board's existing firmware. Back up first, and restore by writing the backup at offset 0:
  `esptool.py --chip esp32s3 read_flash 0 ALL backup.bin` / `esptool.py --chip esp32s3 write_flash 0 backup.bin`
- Do not connect or power a GPS breakout on the XIAO until its receiver, supply voltage, and UART levels are confirmed.

## Signature review notes

Carried over unchanged from upstream; open items for the planned review (registry owners and sources are in the datasets note linked above):
- The OUI list includes broad vendor prefixes: `00:03:7f` is Qualcomm Atheros, and at least `a4:cf:12` and `3c:71:bf` appear to be Espressif, so ESP32 boards (another detector, a test transmitter) can match. All OUI rules are labeled possible.
- The BLE Raven rule matches any 16-bit service UUID in `0x3100`-`0x3500`; labeled specific pending review.
- The BLE names "DfuTarg" and a bare 10-digit serial, the Nordic DFU service, and company ID `0x09C8` are shared by unrelated products; labeled possible.

## License

MIT. Signature data and matching logic are ported from upstream flock-you (`main.cpp`), Copyright (c) 2026 colonelpanichacks; see `LICENSE` in the repository root.
