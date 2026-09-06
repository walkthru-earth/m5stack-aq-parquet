#!/usr/bin/env python3
"""Print the partition table stored inside a saved CoreS3 flash image.

Read-only. Point it at a file produced by `pixi run backup` so you can see what
was on the board before you overwrite it.

Usage
    pixi run backup-partitions backup/cores3-flash-<stamp>.bin
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

# The ESP-IDF partition table always lives at this offset and is one flash
# sector long. Each entry is 32 bytes and starts with this magic.
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x0C00
ENTRY_SIZE = 32
ENTRY_MAGIC = b"\xAA\x50"

TYPES = {0x00: "app", 0x01: "data"}

SUBTYPES = {
    (0x00, 0x00): "factory",
    (0x00, 0x10): "ota_0",
    (0x00, 0x11): "ota_1",
    (0x00, 0x20): "test",
    (0x01, 0x00): "otadata",
    (0x01, 0x01): "phy",
    (0x01, 0x02): "nvs",
    (0x01, 0x04): "nvs_keys",
    (0x01, 0x81): "fat",
    (0x01, 0x82): "spiffs",
    (0x01, 0x83): "littlefs",
}


def parse(image: bytes) -> list[dict]:
    table = image[PARTITION_TABLE_OFFSET : PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE]
    entries = []
    for offset in range(0, len(table), ENTRY_SIZE):
        entry = table[offset : offset + ENTRY_SIZE]
        # The first entry that is not magic terminates the table.
        if entry[:2] != ENTRY_MAGIC:
            break
        ptype, subtype = entry[2], entry[3]
        start, size = struct.unpack("<II", entry[4:12])
        name = entry[12:28].split(b"\x00")[0].decode("utf-8", "replace")
        entries.append(
            {
                "name": name,
                "type": TYPES.get(ptype, hex(ptype)),
                "subtype": SUBTYPES.get((ptype, subtype), hex(subtype)),
                "offset": start,
                "size": size,
            }
        )
    return entries


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"error, no such file, {path}", file=sys.stderr)
        return 1

    image = path.read_bytes()
    if len(image) <= PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE:
        print(f"error, image is too small to contain a partition table, {len(image)} bytes", file=sys.stderr)
        return 1

    entries = parse(image)
    if not entries:
        print("no partition table found at 0x8000, this may not be an ESP-IDF image", file=sys.stderr)
        return 1

    print(f"{path.name}, {len(image) / (1024 * 1024):.0f} MB image")
    print(f"{'name':<16} {'type':<6} {'subtype':<10} {'offset':>10} {'size':>10}")
    for e in entries:
        print(
            f"{e['name']:<16} {e['type']:<6} {e['subtype']:<10} "
            f"{e['offset']:#010x} {e['size'] // 1024:>9d}K"
        )

    end = max(e["offset"] + e["size"] for e in entries)
    print(f"\nhighest partition end, {end:#x}, {end / (1024 * 1024):.2f} MB of {len(image) / (1024 * 1024):.0f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
