#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode a dump of the detector's "obslog" partition into CSV files.

Read the partition from a connected board first (physical USB access; the
board reboots afterwards), for example:

    esptool.py --chip esp32s3 --port <PORT> read_flash 0x310000 0x400000 obslog.bin

Then:

    python read_obslog.py obslog.bin --battery-csv battery.csv --encounters-csv encounters.csv

Encounter rows contain the detector's recorded positions; treat the output as
sensitive location history.
"""

import argparse
import csv
import datetime
import struct
import sys
import zlib

RECORD_SIZE = 128
SECTOR_SIZE = 4096
MAGIC = 0x31455946
VERSION = 1
TYPE_ENCOUNTER = 1
TYPE_BATTERY = 2

# fy::Rule order (fy_signatures.h).
RULES = [
    "wifi_probe_ie_sig", "wifi_wildcard_probe", "wifi_oui_addr2", "wifi_oui_addr1",
    "wifi_oui_addr3", "ble_name_penguin", "ble_name_fs_ext_battery", "ble_name_bare_serial",
    "ble_name_dfutarg", "ble_mfg_09c8", "ble_svc_flock", "ble_svc_nordic_dfu", "ble_svc_raven",
]
SOURCES = {0: "none", 1: "gnss", 2: "phone"}
STATES = {0: "none", 1: "current", 2: "stale"}

HEADER = struct.Struct("<IIIBBBBI")            # magic seq crc version type flags reserved boot
LOCATION = struct.Struct("<iiBBB")            # lat_e7 lon_e7 hdop_x10 age_s source_state
OBSERVATION = struct.Struct("<IIbB11s")       # uptime_ms utc rssi channel location
ENCOUNTER = struct.Struct("<I6sHbbBB21s21s21s29s")
BATTERY = struct.Struct("<IIHHB")


def crc_ok(rec):
    crc = zlib.crc32(rec[12:], zlib.crc32(rec[:8]))
    return crc == struct.unpack_from("<I", rec, 8)[0]


def utc_iso(seconds):
    if not seconds:
        return ""
    return datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def observation(raw, prefix):
    uptime_ms, utc, rssi, channel, loc = OBSERVATION.unpack(raw)
    lat, lon, hdop, age, ss = LOCATION.unpack(loc)
    state = STATES.get(ss >> 4, "?")
    has_pos = state in ("current", "stale")
    return {
        f"{prefix}_uptime_s": f"{uptime_ms / 1000:.1f}",
        f"{prefix}_utc": utc_iso(utc),
        f"{prefix}_rssi": rssi,
        f"{prefix}_channel": channel or "",
        f"{prefix}_fix": state,
        f"{prefix}_source": SOURCES.get(ss & 0x0F, "?"),
        f"{prefix}_lat": f"{lat / 1e7:.7f}" if has_pos else "",
        f"{prefix}_lon": f"{lon / 1e7:.7f}" if has_pos else "",
        f"{prefix}_hdop": f"{hdop / 10:.1f}" if hdop else "",
        f"{prefix}_fix_age_s": age if has_pos else "",
    }


def records(data):
    """Yields (seq, type, flags, boot, payload, raw) for every valid record."""
    for off in range(0, len(data) - RECORD_SIZE + 1, RECORD_SIZE):
        rec = data[off:off + RECORD_SIZE]
        magic, seq, _crc, version, rtype, flags, _res, boot = HEADER.unpack_from(rec)
        if magic != MAGIC or version != VERSION or not crc_ok(rec):
            continue
        yield seq, rtype, flags, boot, rec[HEADER.size:], rec


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump", help="binary dump of the obslog partition")
    ap.add_argument("--battery-csv", help="write battery samples here")
    ap.add_argument("--encounters-csv", help="write encounters here (latest state of each)")
    args = ap.parse_args()

    data = open(args.dump, "rb").read()
    battery, encounters = [], {}
    for seq, rtype, flags, boot, payload, _ in records(data):
        if rtype == TYPE_BATTERY:
            uptime_ms, utc, mv, raw_mv, pct = BATTERY.unpack_from(payload)
            battery.append({
                "seq": seq, "boot": boot, "uptime_s": f"{uptime_ms / 1000:.1f}",
                "utc": utc_iso(utc), "mv": mv, "raw_mv": raw_mv, "percent": pct,
                "plausible": int(bool(flags & 0x01)),
            })
        elif rtype == TYPE_ENCOUNTER:
            (enc, mac, count, rssi_min, rssi_max, rule, _r, first, last, best,
             name) = ENCOUNTER.unpack_from(payload)
            key = (boot, enc)
            if key in encounters and encounters[key]["seq"] > seq:
                continue  # keep the newest snapshot of each encounter
            row = {
                "seq": seq, "boot": boot, "encounter": enc,
                "closed": int(bool(flags & 0x04)),
                "strength": "specific" if flags & 0x01 else "possible",
                "transport": "ble" if flags & 0x02 else "wifi",
                "rule": RULES[rule] if rule < len(RULES) else f"rule_{rule}",
                "mac": ":".join(f"{b:02x}" for b in mac),
                "name": name.split(b"\0")[0].decode("ascii", "replace"),
                "count": count, "rssi_min": rssi_min, "rssi_max": rssi_max,
            }
            for raw, prefix in ((first, "first"), (last, "last"), (best, "best")):
                row.update(observation(raw, prefix))
            encounters[key] = row

    battery.sort(key=lambda r: r["seq"])
    enc_rows = sorted(encounters.values(), key=lambda r: r["seq"])
    print(f"{len(battery)} battery samples, {len(enc_rows)} encounters", file=sys.stderr)

    for path, rows in ((args.battery_csv, battery), (args.encounters_csv, enc_rows)):
        if not path:
            continue
        with open(path, "w", newline="") as f:
            if rows:
                w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
                w.writeheader()
                w.writerows(rows)
        print(f"wrote {path}", file=sys.stderr)


if __name__ == "__main__":
    main()
