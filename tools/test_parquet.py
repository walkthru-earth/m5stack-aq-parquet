"""Cross-reader conformance checks for the exact C++ writer used on the board.

Run with ``pixi run python tools/test_parquet.py [--sanitize]``.
The fixture also checks writer capacity rejection and abort-on-sink-failure.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import subprocess
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq


BASE_NAMES = [
    "time_ms",
    "counter",
    "temperature",
    "optional_counter",
    "all_null",
    "signed_extreme",
    "optional_extreme",
    "all_present",
]
OPTIONAL_NAMES = {
    "optional_counter",
    "all_null",
    "optional_extreme",
    "all_present",
}


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def expected_temperature(index: int, rows: int) -> float:
    if rows > 4 and index < 4:
        return (-0.0, math.inf, -math.inf, math.nan)[index]
    return index * 0.25 - 10.0


def check_float(actual: float, expected: float, description: str) -> None:
    if math.isnan(expected):
        require(actual is not None and math.isnan(actual), description)
    else:
        require(actual == expected, description)
        if expected == 0:
            require(math.copysign(1, actual) == math.copysign(1, expected), description)


def expected_values(index: int, rows: int) -> tuple:
    int32_value = 2**31 - 1 if index % 2 else -(2**31)
    int64_value = 2**63 - 1 if index % 2 else -(2**63)
    return (
        1700000000000 + index * 10000,
        int32_value,
        expected_temperature(index, rows),
        int32_value if index % 2 else None,
        None,
        int64_value,
        int64_value if index % 2 else None,
        expected_temperature(index, rows),
    )


def check_row(actual: tuple, index: int, rows: int, columns: int, reader: str) -> None:
    expected = expected_values(index, rows)
    for col in (0, 1, 3, 4, 5, 6):
        require(actual[col] == expected[col], f"{reader}: row {index}, {BASE_NAMES[col]}")
    for col in (2, 7, *range(8, columns)):
        check_float(actual[col], expected[2], f"{reader}: row {index}, float column {col}")


def check_file(path: Path, rows: int, columns: int) -> None:
    names = BASE_NAMES + [f"extra_{i:02}" for i in range(8, columns)]
    types = [pa.int64(), pa.int32(), pa.float32(), pa.int32(), pa.float32(),
             pa.int64(), pa.int64(), pa.float32()] + [pa.float32()] * (columns - 8)
    parquet = pq.ParquetFile(path)
    metadata = parquet.metadata
    table = parquet.read()
    require(table.num_rows == rows and table.num_columns == columns, "Arrow table dimensions")
    require(table.column_names == names, "Arrow schema names/order")
    for field, name, data_type in zip(table.schema, names, types, strict=True):
        require(field.type == data_type, f"Arrow type: {name}")
        require(field.nullable == (name in OPTIONAL_NAMES), f"Arrow nullability: {name}")
    require(metadata.num_rows == rows, "footer row count")
    require(metadata.num_columns == columns, "footer column count")
    require(metadata.num_row_groups == (1 if rows else 0), "footer row-group count")
    require(metadata.metadata[b"sample_interval_ms"] == b"10000", "interval metadata")
    require(metadata.metadata[b"fixture"] == b"firmware writer interoperability", "fixture metadata")
    require(metadata.created_by == "m5stack-aq-parquet version 0.1", "writer identity")
    if rows:
        group = metadata.row_group(0)
        require(group.num_rows == rows, "row-group rows")
        for index, name in enumerate(names):
            column = group.column(index)
            require(column.path_in_schema == name, "column metadata order")
            require(column.compression == "UNCOMPRESSED", f"codec: {name}")
            require(set(column.encodings) == {"PLAIN", "RLE"}, f"encodings: {name}")
            require(column.num_values == rows, f"value count: {name}")
            nulls = rows if name == "all_null" else ((rows + 1) // 2 if name.startswith("optional_") else 0)
            require(column.statistics.null_count == nulls, f"null statistics: {name}")
            require(not column.statistics.has_min_max, f"unexpected min/max statistics: {name}")
    arrays = [column.to_pylist() for column in table.columns]
    for index, row in enumerate(zip(*arrays, strict=True)):
        check_row(row, index, rows, columns, "PyArrow")
    with duckdb.connect() as connection:
        result = connection.execute("SELECT * FROM read_parquet(?)", [str(path)])
        require([column[0] for column in result.description] == names, "DuckDB schema names/order")
        duck_rows = result.fetchall()
    require(len(duck_rows) == rows, "DuckDB row count")
    for index, row in enumerate(duck_rows):
        require(len(row) == columns, "DuckDB column count")
        check_row(row, index, rows, columns, "DuckDB")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer and UBSan")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="m5-parquet-test-") as temporary:
        directory = Path(temporary)
        executable = directory / "parquet_fixture"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [
            str(root / "tools/parquet_fixture.cpp"),
            str(root / "firmware/arduino-m5unified/bringup/parquet_writer.cpp"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        for label, rows, columns in (
            ("empty", 0, 8),
            ("one", 1, 8),
            ("wide", 90, 96),
            ("large", 65536, 8),
        ):
            path = directory / f"{label}.parquet"
            subprocess.run([str(executable), str(path), str(rows), str(columns)],
                           check=True, stdout=subprocess.DEVNULL)
            check_file(path, rows, columns)
            print(f"PASS {label}: {rows} rows x {columns} columns; PyArrow + DuckDB exact readback", flush=True)
    print("PASS firmware Parquet writer conformance" + (" (ASan + UBSan)" if args.sanitize else ""))


if __name__ == "__main__":
    main()
