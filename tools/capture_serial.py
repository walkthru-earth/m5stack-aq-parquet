#!/usr/bin/env python3
"""Capture serial output from the board for a bounded time, then stop.

Non-interactive on purpose, so a diagnostic dump can be recorded and read back
without a human sitting on a terminal. Writes to stdout and optionally a file.

Usage
    pixi run capture --port /dev/cu.usbmodem101 --seconds 25
    pixi run capture --port /dev/cu.usbmodem101 --seconds 25 --out logs/boot.txt
    pixi run capture --port /dev/cu.usbmodem101 --until "DIAGNOSTIC COMPLETE"

Nothing is ever written to the device except an optional reset pulse, so this is
safe to run against a board you care about.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial is missing, run this through pixi", file=sys.stderr)
    raise SystemExit(1)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="serial device, for example /dev/cu.usbmodem101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=20.0, help="hard upper bound on capture time")
    parser.add_argument("--until", default=None, help="stop early once this text appears")
    parser.add_argument("--out", default=None, help="also write the capture to this file")
    parser.add_argument("--reset", action="store_true", help="pulse DTR/RTS first to reboot the board")
    return parser


def main() -> int:
    args = build_parser().parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:
        print(f"could not open {args.port}, {exc}", file=sys.stderr)
        return 1

    with port:
        if args.reset:
            # Standard esp32 auto-reset pulse. Leaves the chip running the app.
            port.setDTR(False)
            port.setRTS(True)
            time.sleep(0.2)
            port.setRTS(False)
            time.sleep(0.2)
        port.reset_input_buffer()

        collected = bytearray()
        deadline = time.monotonic() + args.seconds
        # The bound is always enforced, so a silent board cannot hang this.
        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                collected += chunk
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
                if args.until and args.until.encode() in collected:
                    print(f"\n[capture] matched {args.until!r}, stopping early", file=sys.stderr)
                    break

    text = collected.decode("utf-8", "replace")
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        print(f"\n[capture] {len(collected)} bytes to {out}", file=sys.stderr)

    if not collected:
        print(
            "\n[capture] nothing received. The board may be in the ROM download "
            "bootloader, which prints nothing, or the app may not be running.",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
