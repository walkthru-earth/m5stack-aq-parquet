"""Export all currently listed finalized files without resetting or flushing.

Use an artifacts/ directory OUTSIDE Arduino build/; compilation can clean build/.
Legacy names are exposed only by firmware supporting the legacy-parquet prefix.
"""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import time

import parquet_device as device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if "build" in args.out.parts:
        parser.error("use artifacts/, not a build directory that Arduino may clean")
    if args.out.is_symlink():
        parser.error("output directory must not be a symbolic link")
    if (args.out / "manifest.json").exists():
        parser.error("choose a new export directory; manifest already exists")
    records = {}
    pattern = re.compile(r"PARQUET FILE name=(\S+) bytes=(\d+)\Z")
    with device.open_port(args.port, 115200) as port:
        for sweep in range(3):
            listed = {}
            device.send_command(port, "parquet list")
            for line in device.lines_until(port, time.monotonic() + 30):
                if line.startswith("PARQUET ERROR"):
                    raise RuntimeError(line)
                if match := pattern.fullmatch(line):
                    listed[device.safe_name(match[1])] = int(match[2])
                if line.startswith("PARQUET PARTIAL"):
                    print(line, flush=True)
                if line == "PARQUET LIST END":
                    break
            else:
                raise TimeoutError("incomplete inventory")
            missing = sorted(set(listed) - set(records))
            if not missing:
                break
            for name in missing:
                device.send_command(port, "parquet get " + name)
                payload = device.receive_file(device.lines_until(port, time.monotonic() + 60), name)
                if len(payload) != listed[name]:
                    raise ValueError("inventory size mismatch: " + name)
                relative = name if name.startswith("legacy-parquet/") else "output/" + name
                summary = device.save_verified(payload, relative, args.out)
                records[name] = {"path": relative, "source": "live-device-readback",
                                 "bytes": len(payload), "rows": summary["rows"],
                                 "crc32": summary["crc32"],
                                 "sha256": hashlib.sha256(payload).hexdigest(),
                                 "readers_match": summary["readers_match"]}
                print(json.dumps(records[name]), flush=True)
        else:
            raise RuntimeError("inventory kept changing; partial export retained, retry with a new directory")
    report = {"completed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "port": args.port, "inventory_rechecked": True,
              "reset_or_flush_requested": False, "device_deletions": False,
              "files": list(records.values()), "total_files": len(records),
              "total_rows": sum(item["rows"] for item in records.values())}
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "manifest.json").open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"PASS {len(records)} files / {report['total_rows']} rows -> {args.out}", flush=True)


if __name__ == "__main__":
    main()
