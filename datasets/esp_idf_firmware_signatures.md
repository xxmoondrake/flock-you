# Limier's signatures

This note lists what Limier ([`firmware/`](../firmware/README.md)) matches, where each identifier comes from, and how it is labeled. Every match is labeled one of two ways, and neither label proves what a device is:
- **Specific:** the identifier points to the gear. A specific match flashes the Heltec's screen.
- **Possible:** a broad manufacturer prefix or a generic identifier.

## Sources

The set starts from upstream flock-you's signatures (`main.cpp` at `6704f01`). It adds the device families of [datafx/alpr_detector](https://github.com/datafx/alpr_detector) (at `35ae012`). Only identifiers were taken from alpr_detector, never code or tables, because it is GPLv3 and this fork is MIT.

Each identifier is checked against one of these sources:
- **IEEE registry:** the MA-L, MA-M, MA-S, and IAB lists. Each prefix is checked at its exact block size.
- **Bluetooth SIG assigned numbers:** company identifiers, member UUIDs, and service UUIDs.
- **Published field captures,** where no registry applies (BLE names, payload tags, Raven services):
  - [soyboi1312/all-cameras-are-beacons](https://github.com/soyboi1312/all-cameras-are-beacons) (`docs/signatures.md` and `docs/axon.md`, Apache-2.0);
  - [nitekry/nite-oui-collection](https://github.com/nitekry/nite-oui-collection);
  - upstream's [firmware_derived_signatures.md](firmware_derived_signatures.md) (a Flock camera firmware dump) and [NitekryDPaul_wifi_ouis.md](NitekryDPaul_wifi_ouis.md).

## Changes from upstream

**Added**
- **Vendor watchlist (78 IEEE blocks).** The watchlist covers ALPR makers, body cameras, two-way radios, in-vehicle networking, speed radar, light bars, drones, and gunshot sensors. It is matched on any Wi-Fi frame and as BLE public addresses.
  - A vendor that sells essentially only this gear is **specific**.
  - A supplier whose blocks also ship in civilian equipment is **possible**.
- **BLE families:**
  - **Axon:** the `BWCDEVICE` payload tag, company ID `0x034D`, and member UUIDs `0xFE6B`, `0xFE6C`, and `0xFC81` (specific).
  - **BodyWorn:** the remote's name (specific).
  - **Motorola Solutions:** company ID `0x04EC` and member UUIDs `0xFD8E` and `0xFE04` (possible).
  - **Flock:** `FS-` plus four or more hex digits, and names starting `Flock` (both possible).
- **Flock's SoftAP SSID:** `Flock-` plus four or more hex digits (specific).

**Changed**
- **Flock Safety's own block** `b4:1e:52` is now specific.
- **Radio-module OUIs** (Lite-On, Silicon Labs, Espressif, Samsung, and USI) also ship in laptops, phones, and IoT gear.
  - By default they are matched only in wildcard probes, where the IE fingerprint tells a camera from a laptop.
  - `FY_SIG_RADIO_OUIS_ANY_FRAME` restores upstream's any-frame matching.
  - Another ESP32 can still match on the Espressif blocks.
- **Raven** is narrowed to the five services it advertises, which are read in both 16-bit and 128-bit form.
- **Vendor switches:** Motorola Solutions and Sierra Wireless are on by default. Cradlepoint is off: in alpr_detector's field tests its matches were school buses and traffic cameras.
- **Shared identifiers are possible:** `DfuTarg`, the Nordic DFU service, and company ID `0x09C8`, because unrelated products share them.

**Removed**
- **`b8:35:32`:** it is not an IEEE assignment.
- **The bare 10-digit BLE name:** it matched a phone. Its rule ID stays so that stored records still decode.

**Not taken from alpr_detector:** its test SSID, and its rule that OUIs match only non-randomized addresses.

## Rules

Wi-Fi OUI rules take the matched vendor's label; the others have a fixed one. The last column is the method and tier reported to upstream's Flask dashboard. BLE names are compared ignoring ASCII case.

| Rule | Matches | Label | Source | Dashboard method (tier) |
|---|---|---|---|---|
| `wifi_probe_ie_sig` | Wildcard probe request from a Flock radio OUI with the drive-tested IE fingerprint (below) | Specific | Community drive tests (@NitekryDPaul, DeFlockJoplin) | `wifi_wildcard_probe_ie_sig` (4) |
| `wifi_wildcard_probe` | The same probe without the fingerprint, from a radio-module OUI | Possible | Same | `wifi_wildcard_probe` (3) |
| `wifi_oui_addr2` | Watchlist OUI as transmitter, any frame | Vendor's | IEEE registry | `wifi_oui_addr2` (2) |
| `wifi_oui_addr1` | Watchlist OUI as receiver | Vendor's | IEEE registry | `wifi_oui_addr1` (1) |
| `wifi_oui_addr3` | Watchlist OUI as BSSID of a management frame | Vendor's | IEEE registry | `wifi_oui_addr3` (1) |
| `wifi_ssid_flock` | SSID `Flock-` plus 4 or more hex digits in a beacon or probe response (a client's probe request for the network names no device) | Specific | Firmware dump (`WifiApService`); ryanohoro; GainSec | `wifi_ssid` (3) |
| `ble_name_penguin` | Name `Penguin-` followed by exactly 10 digits | Specific | Firmware dump; ryanohoro | `ble_name` (2) |
| `ble_name_fs_ext_battery` | Name `FS Ext Battery` | Specific | Firmware dump; ryanohoro | `ble_name` (2) |
| `ble_name_fs_hex` | Name `FS-` followed by 4 or more hex digits | Possible | all-cameras-are-beacons field capture (`FS-BEC46A`); generic white-label prefix | `ble_name` (2) |
| `ble_name_flock` | Name starting `Flock` | Possible | Brand string | `ble_name` (2) |
| `ble_name_bare_serial` | Retired: never matched now | — | — | `ble_name` (2) |
| `ble_name_dfutarg` | Name `DfuTarg`, Nordic's legacy DFU bootloader default | Possible | Upstream; generic Nordic | `ble_name` (2) |
| `ble_mfg_09c8` | Manufacturer data, company ID `0x09C8` | Possible | Bluetooth SIG: "XUNTONG", the battery packs' module vendor | `ble_mfg` (2) |
| `ble_svc_flock` | Service UUID `e8ccbb38-9532-46a8-9fe5-1814df172e6f` | Specific | Firmware dump | `ble_gatt_svc` (2) |
| `ble_svc_nordic_dfu` | Service UUID `00001530-1212-efde-1523-785feabcd123`, Nordic legacy DFU | Possible | Upstream; generic Nordic | `ble_gatt_svc` (2) |
| `ble_svc_raven` | 16-bit service UUID `0x3100`, `0x3200`, `0x3300`, `0x3400`, or `0x3500`, in 16-bit or 128-bit base-UUID form | Specific | all-cameras-are-beacons field capture; firmware dump. Not SIG-assigned | `ble_gatt_svc` (2) |
| `ble_axon_tag` | ASCII `BWCDEVICE`, forwards or reversed, in 128-bit UUID or service-data bytes | Specific | all-cameras-are-beacons field capture, owner-confirmed body cameras | `ble_gatt_svc` (2) |
| `ble_mfg_axon` | Manufacturer data, company ID `0x034D` | Specific | Bluetooth SIG: "TASER International, Inc." | `ble_mfg` (2) |
| `ble_svc_axon` | 16-bit service UUID `0xFE6B`, `0xFE6C`, or `0xFC81` (lists or service data) | Specific | Bluetooth SIG: TASER International (`FE6B`, `FE6C`), Axon Enterprise (`FC81`) | `ble_gatt_svc` (2) |
| `ble_name_bodyworn` | Name starting `BodyWorn Remote` | Specific | nite-oui-collection capture (via all-cameras-are-beacons) | `ble_name` (2) |
| `ble_mfg_motorola` | Manufacturer data, company ID `0x04EC` | Possible | Bluetooth SIG: "Motorola Solutions" | `ble_mfg` (2) |
| `ble_svc_motorola` | 16-bit service UUID `0xFD8E` or `0xFE04` | Possible | Bluetooth SIG: Motorola Solutions | `ble_gatt_svc` (2) |
| `ble_oui` | Watchlist OUI as a BLE public device address (random addresses are never matched) | Vendor's | IEEE registry | `ble_oui` (1) |

When an advert carries several signatures, the strongest wins: any specific one over a possible one, then name, payload tag, services, manufacturer data, and address in that order. Not matched: upstream's disabled SSID keyword path, the generic classic-Bluetooth names `msm8953_32` and `Android`, and the standard SIG services Raven also advertises (`0x180A`, `0x1809`, `0x1819`).

## Probe IE fingerprint

`wifi_probe_ie_sig` compares the probe body's information-element layout with `2,12,127,221:506f9a16030103,45,191,221:0050f208000000`. Vendor elements are written as `221:` plus their OUI and leading bytes. In the IEEE registry, `50:6f:9a` is the Wi-Fi Alliance and `00:50:f2` is Microsoft. Both are common to many Wi-Fi stacks, so the fingerprint's precision comes from the whole layout, not from either vendor element.

## Vendor watchlist (78)

Matched on any frame and as BLE public addresses, at the label shown. Name collisions are deliberately absent: `00:01:21` and `00:90:7F` (WatchGuard Technologies firewalls, not WatchGuard Video), `24:A3:F0:7` and `00:1B:C5:0B:4` (Coban SRL, not COBAN Technologies), `8C:1F:64:A7:8` (TAIT Global LLC, not Tait Electronics), `00:58:28`, `00:C0:D4`, `84:70:03` (Axon Networks, not Axon Enterprise), and `D4:63:52` (Vutility, not Utility Inc.). So are blocks too broad to mean anything: Dell, Panasonic, Peplink (`D4:13:F8`), and Pepwave (`A8:C0:EA`).

| Prefix | IEEE block and registrant | Vendor | Category | Label | Switch |
|---|---|---|---|---|---|
| `00:00:C3` /24 | MA-L Harris Corporation | Harris | Two-way radio | Possible |  |
| `00:04:4C` /24 | MA-L JENOPTIK | Jenoptik | ALPR | Possible |  |
| `00:04:7D` /24 | MA-L Motorola Solutions Inc. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `00:06:EC` /24 | MA-L Harris Corporation | Harris | Two-way radio | Possible |  |
| `00:08:B8` /24 | MA-L E.F. Johnson | E.F. Johnson | Two-way radio | Specific |  |
| `00:09:BC` /24 | MA-L Utility, Inc | Utility Inc. (BodyWorn) | Body camera | Specific |  |
| `00:0A:3E` /24 | MA-L EADS Telecom | EADS Telecom | Two-way radio | Possible |  |
| `00:0D:4F` /24 | MA-L Kenwood Corporation | Kenwood | Two-way radio | Possible |  |
| `00:0D:CA` /24 | MA-L Tait Electronics | Tait Electronics | Two-way radio | Possible |  |
| `00:0E:06` /24 | MA-L Team Simoco Ltd | Team Simoco | Two-way radio | Possible |  |
| `00:12:E0` /24 | MA-L Codan Limited | Codan | Two-way radio | Possible |  |
| `00:14:3E` /24 | MA-L AirLink Communications, Inc. | AirLink Communications | Vehicle network | Possible |  |
| `00:14:91` /24 | MA-L Daniels Electronics Ltd. dba Codan Radio Communications | Codan Radio | Two-way radio | Possible |  |
| `00:16:ED` /24 | MA-L Utility, Inc | Utility Inc. (BodyWorn) | Body camera | Specific |  |
| `00:17:28` /24 | MA-L Selex Communications | Selex Communications | Two-way radio | Possible |  |
| `00:17:3D` /24 | MA-L Neology | Neology | ALPR | Possible |  |
| `00:17:F3` /24 | MA-L Harris Corporation | Harris | Two-way radio | Possible |  |
| `00:18:85` /24 | MA-L Motorola Solutions Inc. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `00:1A:08` /24 | MA-L Simoco Ltd. | Simoco | Two-way radio | Possible |  |
| `00:1C:3C` /24 | MA-L Seon Design Inc. | Seon Design | Body camera | Specific |  |
| `00:1D:96` /24 | MA-L WatchGuard Video | WatchGuard Video | Body camera | Specific |  |
| `00:1E:96` /24 | MA-L Sepura Limited | Sepura | Two-way radio | Possible |  |
| `00:1F:92` /24 | MA-L Motorola Solutions Inc. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `00:1F:9C` /24 | MA-L Havis Inc. | Havis | Vehicle network | Possible |  |
| `00:22:AF` /24 | MA-L Safety Vision, LLC | Safety Vision | Body camera | Possible |  |
| `00:23:B9` /24 | MA-L Airbus Defence and Space Deutschland GmbH | Airbus Defence and Space | Two-way radio | Possible |  |
| `00:23:BD` /24 | MA-L Digital Ally, Inc. | Digital Ally | Body camera | Specific |  |
| `00:24:39` /24 | MA-L Digital Barriers Advanced Technologies | Digital Barriers | Body camera | Specific |  |
| `00:24:E6` /24 | MA-L In Motion Technology Inc. | In Motion Technology | Vehicle network | Possible |  |
| `00:25:DF` /24 | MA-L Axon Enterprise, Inc. | Axon | Body camera | Specific |  |
| `00:26:B3` /24 | MA-L Thales Communications Inc | Thales Communications | Two-way radio | Possible |  |
| `00:30:44` /24 | MA-L CradlePoint, Inc | Cradlepoint | Vehicle network | Possible | `FY_SIG_CRADLEPOINT` (off by default) |
| `00:30:7E` /24 | MA-L Redflex Communication Systems | Redflex | ALPR | Specific |  |
| `00:40:DE` /24 | MA-L Elsag Datamat spa | Elsag Datamat | ALPR | Possible |  |
| `00:50:C2:BE:7` /36 | IAB Genetec Inc. | Genetec | ALPR | Possible |  |
| `00:90:C7` /24 | MA-L ICOM INC. | Icom | Two-way radio | Possible |  |
| `00:A0:D5` /24 | MA-L Sierra Wireless, ULC | Sierra Wireless | Vehicle network | Possible | `FY_SIG_SIERRA_WIRELESS` |
| `00:BF:15` /24 | MA-L Genetec Inc. | Genetec | ALPR | Possible |  |
| `00:E0:1C` /24 | MA-L CradlePoint, Inc | Cradlepoint | Vehicle network | Possible | `FY_SIG_CRADLEPOINT` (off by default) |
| `00:E0:6A` /24 | MA-L KAPSCH AG | Kapsch | ALPR | Possible |  |
| `04:C3:E6:9` /28 | MA-M Ekin Teknoloji San ve Tic A.S. | Ekin Teknoloji | ALPR | Possible |  |
| `08:3C:03:0` /28 | MA-M Federal Signal SSG | Federal Signal | Light bar | Specific |  |
| `0C:BF:15` /24 | MA-L Genetec Inc. | Genetec | ALPR | Possible |  |
| `10:74:6F` /24 | MA-L MOTOROLA SOLUTIONS MALAYSIA SDN. BHD. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `1C:82:59:D` /28 | MA-M Applied Concepts, Inc. | Applied Concepts (Stalker) | Speed radar | Specific |  |
| `28:A3:31` /24 | MA-L Sierra Wireless, ULC | Sierra Wireless | Vehicle network | Possible | `FY_SIG_SIERRA_WIRELESS` |
| `38:1D:14` /24 | MA-L Skydio Inc. | Skydio | Drone | Possible |  |
| `38:73:EA:0` /28 | MA-M L-3 Communications Mobile-Vision, Inc. | L-3 Mobile-Vision | Body camera | Specific |  |
| `48:46:8D` /24 | MA-L Zepcam B.V. | Zepcam | Body camera | Specific |  |
| `48:E3:C3` /24 | MA-L JENOPTIK Advanced Systems GmbH | Jenoptik | ALPR | Possible |  |
| `4C:CC:34` /24 | MA-L Motorola Solutions Inc. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `50:13:9D` /24 | MA-L Sierra Wireless, ULC | Sierra Wireless | Vehicle network | Possible | `FY_SIG_SIERRA_WIRELESS` |
| `58:94:CF` /24 | MA-L Vertex Standard LMR, Inc. | Vertex Standard LMR | Two-way radio | Possible |  |
| `58:E8:76:C` /28 | MA-M KUSTOM SIGNALS INC | Kustom Signals | Speed radar | Specific |  |
| `64:69:BC` /24 | MA-L Hytera Communications Co .,ltd | Hytera | Two-way radio | Possible |  |
| `64:CE:6E` /24 | MA-L Sierra Wireless, ULC | Sierra Wireless | Vehicle network | Possible | `FY_SIG_SIERRA_WIRELESS` |
| `68:DA:73:B` /28 | MA-M Gamber-Johnson LLC | Gamber-Johnson | Vehicle network | Possible |  |
| `6C:18:11` /24 | MA-L Decatur Electronics | Decatur Electronics | Speed radar | Specific |  |
| `70:1A:D5` /24 | MA-L Avigilon Alta | Avigilon Alta | ALPR | Possible |  |
| `70:B3:D5:1C:5` /36 | MA-S ELSAG | ELSAG | ALPR | Specific |  |
| `70:B3:D5:52:1` /36 | MA-S Selex ES Inc. | Selex ES | ALPR | Possible |  |
| `70:B3:D5:88:A` /36 | MA-S Perceptics, LLC | Perceptics | ALPR | Specific |  |
| `70:B3:D5:F5:E` /36 | MA-S Selex ES Inc. | Selex ES | ALPR | Possible |  |
| `84:DB:2F` /24 | MA-L Sierra Wireless, ULC | Sierra Wireless | Vehicle network | Possible | `FY_SIG_SIERRA_WIRELESS` |
| `94:7B:BE` /24 | MA-L Ubicquia LLC | Ubicquia | ALPR | Possible |  |
| `9C:06:6E` /24 | MA-L Hytera Communications Corporation Limited | Hytera | Two-way radio | Possible |  |
| `9C:83:BF` /24 | MA-L PRO-VISION, Inc. | PRO-VISION | Body camera | Specific |  |
| `9C:86:2B` /24 | MA-L MOTOROLA SOLUTIONS MALAYSIA SDN. BHD. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `B4:1E:52` /24 | MA-L Flock Safety | Flock Safety | Flock | Specific |  |
| `B8:E2:8C` /24 | MA-L MOTOROLA SOLUTIONS MALAYSIA SDN. BHD. | Motorola Solutions | Two-way radio | Possible | `FY_SIG_MOTOROLA` |
| `BC:AD:90` /24 | MA-L Kymeta Purchasing | Kymeta | Vehicle network | Possible |  |
| `CC:93:4A` /24 | MA-L Sierra Wireless, ULC | Sierra Wireless | Vehicle network | Possible | `FY_SIG_SIERRA_WIRELESS` |
| `D4:11:D6` /24 | MA-L ShotSpotter, Inc. | ShotSpotter | Gunshot sensor | Specific |  |
| `D8:1F:65` /24 | MA-L, registrant "Private"; attributed to Axon by field capture | Axon (field-attributed) | Body camera | Specific |  |
| `E0:DA:DC` /24 | MA-L JVC KENWOOD Corporation | JVC Kenwood | Two-way radio | Possible |  |
| `E4:1E:0A:B` /28 | MA-M Safety Vision, LLC | Safety Vision | Body camera | Possible |  |
| `F4:69:D5:7` /28 | MA-M Rosco, Inc | Rosco | Body camera | Possible |  |
| `FC:01:9E` /24 | MA-L VIEVU | VIEVU | Body camera | Specific |  |

## Flock radio-module OUIs (33)

Matched only in wildcard probe requests by default, where the IE fingerprint decides between `wifi_probe_ie_sig` (specific) and `wifi_wildcard_probe` (possible); `FY_SIG_RADIO_OUIS_ANY_FRAME` also matches them on any frame, labeled possible. Registrants are from the IEEE registry.

| Prefix | IEEE registrant | Source |
|---|---|---|
| `70:c9:4e` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `3c:91:80` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `d8:f3:bc` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `80:30:49` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `14:5a:fc` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `74:4c:a1` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `08:3a:88` | Universal Global Scientific Industrial., Ltd | Community (@NitekryDPaul) |
| `9c:2f:9d` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `c0:35:32` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `94:08:53` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `e4:aa:ea` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `f4:6a:dd` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `e0:0a:f6` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `24:b2:b9` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `00:f4:8d` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `d0:39:57` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `e8:d0:fc` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `e0:4f:43` | Universal Global Scientific Industrial., Ltd | Community (@NitekryDPaul) |
| `b8:1e:a4` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `70:08:94` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `58:8e:81` | Silicon Laboratories | Community (@NitekryDPaul) |
| `ec:1b:bd` | Silicon Laboratories | Community (@NitekryDPaul) |
| `3c:71:bf` | Espressif Inc. | Community (@NitekryDPaul) |
| `58:00:e3` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `90:35:ea` | Silicon Laboratories | Community (@NitekryDPaul) |
| `5c:93:a2` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `64:6e:69` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `48:27:ea` | Samsung Electronics Co.,Ltd | Community (@NitekryDPaul) |
| `a4:cf:12` | Espressif Inc. | Community (@NitekryDPaul) |
| `14:b5:cd` | Liteon Technology Corporation | Community (@NitekryDPaul) |
| `82:6b:f2` | none: locally administered, never assigned | DeFlockJoplin drive test |
| `b4:1e:52` | Flock Safety | Firmware dump (Flock's own block) |
| `00:03:7f` | Atheros Communications, Inc. | Firmware dump (QCA9377 default MACs) |
