"""Bounded identical-row UNCOMPRESSED/LZ4_RAW SD comparison on CoreS3.

Does not reset, sync time or change the normal codec/interval. Benchmark copies
are marked duplicates under output/benchmarks; normal buffered rows are retained.
"""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import time

import pyarrow.parquet as pq
import parquet_device as device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--min-rows", type=int, default=60)
    parser.add_argument("--seconds", type=device.positive_seconds, default=700)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.min_rows < 90 or not 1 <= args.repeats <= 10:
        parser.error("min-rows must be 1..89 and repeats 1..10")
    if "build" in args.out.parts or args.out.exists():
        parser.error("choose a new artifacts directory outside build/")
    args.out.mkdir(parents=True)
    results = []
    with (args.out / "device.log").open("x") as log, device.open_port(args.port, 115200) as port:
        def lines(deadline):
            for line in device.lines_until(port, deadline):
                log.write(line + "\n")
                log.flush()
                if not line.startswith("PARQUET DATA offset="):
                    print(line, flush=True)
                if line.startswith("PARQUET ERROR"):
                    raise RuntimeError(line)
                yield line

        deadline = time.monotonic() + args.seconds
        enough = False
        while time.monotonic() < deadline:
            device.send_command(port, "parquet status")
            for line in lines(min(deadline, time.monotonic() + 10)):
                if line.startswith("PARQUET STATUS"):
                    match = re.search(r"buffered=(\d+)", line)
                    enough = match is not None and int(match[1]) >= args.min_rows
                    break
            if enough:
                break
            for _ in lines(min(deadline, time.monotonic() + 20)):
                pass
        if not enough:
            raise TimeoutError("requested buffered row count not reached; log retained")
        for attempt in range(args.repeats):
            device.send_command(port, "parquet codec-test")
            ready = []
            for line in lines(time.monotonic() + 60):
                if line.startswith("PARQUET READY"):
                    fields = dict(re.findall(r"(\w+)=(\S+)", line))
                    if fields.get("name", "").startswith("benchmarks/"):
                        ready.append(fields)
                if line.startswith("PARQUET BENCH END"):
                    break
            else:
                raise TimeoutError("no benchmark completion")
            if len(ready) != 2 or [r.get("codec") for r in ready] != ["UNCOMPRESSED", "LZ4_RAW"]:
                raise ValueError("expected one comparison pair")
            tables = []
            for record in ready:
                name = device.safe_name(record["name"])
                device.send_command(port, "parquet get " + name)
                payload = device.receive_file(lines(time.monotonic() + 60), name)
                summary = device.save_verified(payload, name, args.out)
                record.update(sha256=hashlib.sha256(payload).hexdigest(),
                              readers_match=summary["readers_match"])
                tables.append(pq.ParquetFile(summary["path"]).read())
            if not tables[0].equals(tables[1], check_metadata=False):
                raise ValueError("compressed values differ from the identical-row baseline")
            for key in ("rows_dropped", "sample_deadlines_missed", "storage_errors"):
                if any(value != 0 for value in tables[0][key].to_pylist()):
                    raise ValueError(f"nonzero recorded health counter: {key}")
            results.append({"attempt": attempt, "identical_rows": True, "files": ready})
            print(f"PASS identical-row codec pair {attempt + 1}", flush=True)
        device.send_command(port, "parquet status")
        for line in lines(time.monotonic() + 10):
            if line.startswith("PARQUET STATUS"):
                final_status = line
                break
        else:
            raise TimeoutError("no final status")
    report = {"completed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "scope": "real buffered sensor rows; repeated SD finalization; no radio load",
              "comparisons": results, "final_status": final_status}
    with (args.out / "report.json").open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
