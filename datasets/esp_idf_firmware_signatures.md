# Signatures in the ESP-IDF firmware

What the [`firmware/`](../firmware/README.md) build matches, where each identifier comes from, and how it is labeled. The signature set is upstream's, ported unchanged from `main.cpp` at commit `6704f01`. This fork adds the **specific** and **possible** labels and the registry notes below. Primary sources are [firmware_derived_signatures.md](firmware_derived_signatures.md) (Flock camera firmware dump) and [NitekryDPaul_wifi_ouis.md](NitekryDPaul_wifi_ouis.md) (community field research).

A **specific** rule may raise a prominent alert. A **possible** rule rests on a broad manufacturer prefix or a generic identifier. Neither label proves what a device is.

## Rules

The last column is the method and tier the firmware reports to upstream's Flask dashboard. BLE names are compared ignoring ASCII case.

| Rule | Matches | Label | Source | Dashboard method (tier) |
|---|---|---|---|---|
| `wifi_probe_ie_sig` | Probe request from a target OUI with a wildcard SSID and the drive-tested IE fingerprint (below) | Specific | Community drive tests (@NitekryDPaul, DeFlockJoplin) | `wifi_wildcard_probe_ie_sig` (4) |
| `wifi_wildcard_probe` | The same probe without the fingerprint | Possible | Same | `wifi_wildcard_probe` (3) |
| `wifi_oui_addr2` | Target OUI as transmitter, any frame | Possible | OUI list | `wifi_oui_addr2` (2) |
| `wifi_oui_addr1` | Target OUI as receiver | Possible | OUI list | `wifi_oui_addr1` (1) |
| `wifi_oui_addr3` | Target OUI as BSSID of a management frame | Possible | OUI list | `wifi_oui_addr3` (1) |
| `ble_name_penguin` | Name `Penguin-` followed by exactly 10 digits | Specific | Firmware dump | `ble_name` (2) |
| `ble_name_fs_ext_battery` | Name `FS Ext Battery` | Specific | Firmware dump | `ble_name` (2) |
| `ble_name_bare_serial` | Name of exactly 10 digits | Possible | Firmware dump; generic form | `ble_name` (2) |
| `ble_name_dfutarg` | Name `DfuTarg`, Nordic's legacy DFU bootloader default (a pack being updated) | Possible | Upstream `main.cpp`; generic Nordic | `ble_name` (2) |
| `ble_mfg_09c8` | Manufacturer data with company ID `0x09C8` (XUNTONG per upstream) | Possible | Firmware dump | `ble_mfg` (2) |
| `ble_svc_flock` | Service UUID `e8ccbb38-9532-46a8-9fe5-1814df172e6f` | Specific | Firmware dump | `ble_gatt_svc` (2) |
| `ble_svc_nordic_dfu` | Service UUID `00001530-1212-efde-1523-785feabcd123`, Nordic legacy DFU | Possible | Upstream `main.cpp`; generic Nordic | `ble_gatt_svc` (2) |
| `ble_svc_raven` | Any 16-bit service UUID in `0x3100`-`0x3500` | Specific, pending review | Firmware dump (Raven) | `ble_gatt_svc` (2) |

Not matched: upstream's disabled SSID keyword path, and the generic classic-Bluetooth names `msm8953_32` and `Android`.

## Probe IE fingerprint

`wifi_probe_ie_sig` compares the probe body's information-element layout with `2,12,127,221:506f9a16030103,45,191,221:0050f208000000`. Vendor elements are written as `221:` plus their OUI and leading bytes. In the registry copy (see below), `50:6f:9a` is the Wi-Fi Alliance and `00:50:f2` is Microsoft. Both are common to many Wi-Fi stacks, so the fingerprint's precision comes from the whole layout, not from either vendor element. Upstream's code comment attributes `50:6f:9a` to the LiteON and Flock stack.

## Target OUIs (34)

"Registry owner" comes from the IEEE MA-L registry copy in upstream's `oui.txt`, which is undated. Check the live registry during the planned review. Totals: Lite-On Technology 22, Silicon Laboratories 3, Not listed 2, Universal Global Scientific Industrial (USI) 2, Espressif 2, Samsung Electronics 1, Flock Safety 1, Atheros Communications (Qualcomm Atheros) 1.

| OUI | Source | Registry owner | Notes |
|---|---|---|---|
| `70:c9:4e` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `3c:91:80` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `d8:f3:bc` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `80:30:49` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `b8:35:32` | Community (@NitekryDPaul) | Not listed | Not in the registry copy; may be unassigned, newer than the copy, or a transcription error. Review first. |
| `14:5a:fc` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `74:4c:a1` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `08:3a:88` | Community (@NitekryDPaul) | Universal Global Scientific Industrial (USI) |  |
| `9c:2f:9d` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `c0:35:32` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `94:08:53` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `e4:aa:ea` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `f4:6a:dd` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `e0:0a:f6` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `24:b2:b9` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `00:f4:8d` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `d0:39:57` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `e8:d0:fc` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `e0:4f:43` | Community (@NitekryDPaul) | Universal Global Scientific Industrial (USI) |  |
| `b8:1e:a4` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `70:08:94` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `58:8e:81` | Community (@NitekryDPaul) | Silicon Laboratories |  |
| `ec:1b:bd` | Community (@NitekryDPaul) | Silicon Laboratories |  |
| `3c:71:bf` | Community (@NitekryDPaul) | Espressif | Espressif: any ESP32, including another detector or a test transmitter, can match. |
| `58:00:e3` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `90:35:ea` | Community (@NitekryDPaul) | Silicon Laboratories |  |
| `5c:93:a2` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `64:6e:69` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `48:27:ea` | Community (@NitekryDPaul) | Samsung Electronics |  |
| `a4:cf:12` | Community (@NitekryDPaul) | Espressif | Espressif: any ESP32, including another detector or a test transmitter, can match. |
| `14:b5:cd` | Community (@NitekryDPaul) | Lite-On Technology |  |
| `82:6b:f2` | Community (DeFlockJoplin) | Not listed | Locally administered (bit 0x02 of the first byte), so never registry-assigned. Added by DeFlockJoplin. |
| `b4:1e:52` | Firmware dump | Flock Safety | Flock Safety's own assignment. |
| `00:03:7f` | Firmware dump | Atheros Communications (Qualcomm Atheros) | Default MACs in the camera's QCA9377 radio firmware; also common in other Qualcomm Atheros products. |

Because most prefixes belong to module makers whose radios ship in many unrelated products, every OUI rule is labeled possible.

## Planned review

- Reconcile with the device families in datafx/alpr_detector, taking identifiers only. Check each against a primary source: the IEEE and Bluetooth SIG registries, or field captures.
- Confirm every OUI against the live IEEE registry, starting with `b8:35:32`.
- Confirm company ID `0x09C8` against the Bluetooth SIG assigned numbers.
- Decide whether broad Lite-On, Silicon Labs, Espressif, Samsung, and USI prefixes stay in the list, and whether `ble_svc_raven`'s range should narrow before it stays specific.
