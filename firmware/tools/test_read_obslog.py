#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression test for read_obslog.py: advertised names in a dump must not
become spreadsheet formulas in its CSV output.

    python tools/test_read_obslog.py
"""

import csv
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

TOOL = Path(__file__).with_name("read_obslog.py")
HEADER = struct.Struct("<IIIBBBBI")
LOCATION = struct.Struct("<iiBBB")
OBSERVATION = struct.Struct("<IIbB11s")
ENCOUNTER = struct.Struct("<I6sHbbBB21s21s21s29s")

# (advertised name bytes, expected CSV text)
CASES = [
    (b"=1+1", "'=1+1"),
    (b"+1", "'+1"),
    (b"-1", "'-1"),
    (b"@SUM(1)", "'@SUM(1)"),
    (b"\t=1+1", "?=1+1"),     # a control character cannot hide the formula
    (b"\x01=1+1", "?=1+1"),
    (b"a\nb", "a?b"),
    (b'q"uote', 'q"uote'),
    (b"x,y", "x,y"),
    (b"\xe9t\xe9", "?t?"),
    (b"Penguin-123", "Penguin-123"),
    (b"", ""),
]


def record(seq, encounter, name):
    """A valid encounter record (sealed with the firmware's CRC layout)."""
    loc = LOCATION.pack(0, 0, 0, 0, 0)
    obs = OBSERVATION.pack(1000 * encounter, 0, -70, 6, loc)
    mac = bytes([0x02, 0x11, 0x22, 0x33, 0x44, encounter])
    payload = ENCOUNTER.pack(encounter, mac, 3, -80, -60, 0, 0, obs, obs, obs, name.ljust(29, b"\0"))
    rec = bytearray(HEADER.pack(0x31455946, seq, 0, 1, 1, 0x04, 0, 5) + payload)
    struct.pack_into("<I", rec, 8, zlib.crc32(bytes(rec[12:]), zlib.crc32(bytes(rec[:8]))))
    return bytes(rec)


class DecoderCsvTest(unittest.TestCase):
    def test_names_cannot_become_formulas(self):
        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp) / "obslog.bin"
            data = b"".join(record(i + 1, i + 1, name) for i, (name, _) in enumerate(CASES))
            dump.write_bytes(data + b"\xff" * (4096 - len(data)))
            digest = hashlib.sha256(dump.read_bytes()).hexdigest()
            out = Path(tmp) / "encounters.csv"
            subprocess.run([sys.executable, "-I", str(TOOL), str(dump), "--encounters-csv", str(out)],
                           check=True, capture_output=True)

            self.assertEqual(digest, hashlib.sha256(dump.read_bytes()).hexdigest(),
                             "the input dump must not change")
            with open(out, newline="") as f:
                rows = sorted(csv.DictReader(f), key=lambda r: int(r["seq"]))
            self.assertEqual(len(CASES), len(rows))
            columns = len(rows[0])
            for row, (raw, expected) in zip(rows, CASES):
                with self.subTest(name=raw):
                    self.assertEqual(expected, row["name"])
                    self.assertNotIn(row["name"][:1], ("=", "+", "-", "@", "\t", "\r"))
                    self.assertEqual(columns, len(row))
                    for field in ("seq", "boot", "encounter", "count", "first_channel"):
                        self.assertTrue(row[field].isdigit(), field)
                    for field in ("rssi_min", "rssi_max", "best_rssi"):
                        int(row[field])  # still numeric


if __name__ == "__main__":
    unittest.main()
