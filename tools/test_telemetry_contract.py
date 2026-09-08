"""Test the exact firmware dictionary, row layout and clock mapping on the host.

Optional --dictionary-out exports a machine-readable dictionary exclusively.
Synthetic fixtures do not establish device timing, SD durability or OGC compliance.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import parquet_device


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_file(path: Path, dictionary: dict, anchored: bool, compressed: bool) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    fields = dictionary["fields"]
    require(table.num_rows == 90, "row count")
    require(table.column_names == [f["name"] for f in fields], "dictionary/schema order")
    types = {1: pa.int32(), 2: pa.int64(), 4: pa.float32()}
    for field, definition in zip(table.schema, fields, strict=True):
        require(field.type == types[definition["type"]] and field.nullable,
                f"physical schema: {field.name}")
    metadata = parquet.metadata.metadata
    require(metadata[b"schema_version"].decode() == dictionary["schema"], "schema version")
    require(metadata[b"dictionary_sha256"].decode() == dictionary["sha256"], "dictionary digest")
    require(metadata[b"deployment_id"] == metadata[b"calibration_id"] == b"unknown",
            "no fabricated deployment/calibration")
    config = json.loads(metadata[b"acquisition_config"])
    require(config["sample_interval_ms"] == 10000 and config["pms_stale_after_ms"] == 5000,
            "configuration contract")
    for i in range(77):
        require(parquet.metadata.row_group(0).column(i).compression ==
                ("LZ4" if compressed else "UNCOMPRESSED"), "column codec")
    for i, row in enumerate(table.to_pylist()):
        now = 20000000 + i * 10000000
        require(row["schema_version"] == 2 and row["sequence"] == i, "identity")
        require(row["collection_completed_mono_us"] == now + 1234, "completion time")
        require(row["pms_received_mono_us"] == (now - 250000 if i else None), "PMS receipt")
        require(row["clock_anchor_mono_us"] == (15000000 if anchored else None), "anchor mono")
        require(row["clock_anchor_utc_ns"] == (1788890000000000000 if anchored else None), "anchor UTC")
        require(row["event_time_utc_ns"] ==
                (1788890000000000000 + (now - 15000000) * 1000 if anchored else None), "UTC estimate")
        require(row["clock_epoch"] == row["clock_status"] == int(anchored), "clock status")
        require(row["ambient_temperature_c"] is None and row["battery_current_ma"] is None,
                "unsupported measurements stay null")
    with duckdb.connect() as connection:
        other = connection.execute("SELECT * FROM read_parquet(?, hive_partitioning=false)",
                                   [str(path)]).to_arrow_table()
    require(table.equals(other, check_metadata=False), "PyArrow/DuckDB values and nulls")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--dictionary-out", type=Path)
    args = parser.parse_args()
    # Unrelated asynchronous status lines must not terminate a schema reply.
    response = iter(["PARQUET STATUS buffered=2", "PARQUET SCHEMA BEGIN",
                     "PARQUET FIELD name=sequence", "PARQUET SCHEMA END", "AFTER"])
    with patch.object(parquet_device, "send_command") as send, \
            patch.object(parquet_device, "lines_until", return_value=response), \
            contextlib.redirect_stdout(io.StringIO()):
        require(parquet_device.command(None, SimpleNamespace(text="parquet schema", timeout=1)) == 0,
                "schema command completion")
        send.assert_called_once_with(None, "parquet schema")
        require(next(response) == "AFTER", "schema command consumes through END only")
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware/arduino-m5unified/bringup"
    with tempfile.TemporaryDirectory(prefix="m5-contract-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [str(root / "tools/telemetry_contract_fixture.cpp"),
                    str(firmware / "parquet_writer.cpp"), str(firmware / "lz4_codec.cpp"),
                    "-o", str(executable)]
        subprocess.run(command, check=True)
        dictionary = json.loads(subprocess.check_output([str(executable), "dictionary"], text=True))
        require(dictionary["sha256"] == hashlib.sha256((firmware / "telemetry_fields.inc").read_bytes()).hexdigest(),
                "stale compiled dictionary digest")
        fields = dictionary["fields"]
        baseline = [(f["name"], f["type"]) for f in fields[:73]]
        # Golden names/types/order from the 47d8f83 hardware-baseline schema.
        require(hashlib.sha256(json.dumps(baseline, separators=(",", ":")).encode()).hexdigest()
                == "55506b55208628afffbba8d7ef0f82df7469b62969e130263a3cf1cdb38aff16",
                "original 73-column schema changed")
        require(len(fields) == len({f["name"] for f in fields}) == 77, "unique dictionary fields")
        require(len({f["property"] for f in fields}) == 77, "unique property identifiers")
        for field in fields:
            require(re.fullmatch(r"[a-z][a-z0-9_]*", field["name"]) is not None, "safe field token")
            require(all(field[key] for key in ("procedure", "unit", "validity")), "complete metadata")
            require(field["property"] == "urn:walkthru-earth:cores3:property:" + field["name"], "local vocabulary")
        for anchored in (False, True):
            for compressed in (False, True):
                path = directory / f"{anchored}-{compressed}.parquet"
                subprocess.run([str(executable), str(path), "lz4" if compressed else "none",
                                "anchored" if anchored else "unsynced"], check=True)
                check_file(path, dictionary, anchored, compressed)
                print(f"PASS 90 x 77 contract: anchored={anchored} lz4={compressed}; both readers", flush=True)
        if args.dictionary_out:
            with args.dictionary_out.open("x") as output:
                json.dump(dictionary, output, indent=2)
                output.write("\n")
    print("PASS dictionary digest, schema, nulls, clock correction and provenance")


if __name__ == "__main__":
    main()
