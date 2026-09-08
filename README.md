# m5stack-aq-parquet

Air-quality logging firmware for **M5Stack CoreS3 (ESP32-S3)**. The device generates Parquet directly from real measurements and stores finalized files on microSD. Host tools validate and retrieve those files without converting them; object-storage upload is later work.

The repo can hold **more than one framework trial** against the same board, sharing one set of hardware reference docs. It runs one trial at a time, and a second is opened only when a measured result justifies it.

- `AGENTS.md` is the entry point for humans and coding agents.
- `docs/` holds framework-neutral CoreS3 references, including the [telemetry and Parquet pipeline](docs/telemetry-pipeline.md).
- `firmware/<framework>-<variant>/` holds each self-contained trial.

## Current status

As of **2026-09-08**, the only active trial is [Arduino-ESP32 with M5Unified](firmware/arduino-m5unified/README.md), pinned to Arduino-ESP32 3.3.11, M5Unified 0.2.21 and M5GFX 0.2.28. The flashed firmware records one scalar snapshot every **10 seconds**, using a **77-column** schema with explicit nulls/status for unavailable measurements. The PMS display retains its three touch-navigable pages.

The current revision is **77-column schema v2**, with a SHA-256-identified measurement dictionary, configuration/provenance metadata and four additional timing fields. It is flashed and passed short UNCOMPRESSED/unsynced and LZ4/UTC SD readbacks in both readers; [schema-v2 bench evidence](docs/bench-verified.md#board-1-schema-v2-provenance-and-timing) records hashes and limits. Earlier full-window/compression numbers below remain tied to the 73-column image. [Iceberg/OGC decisions, Mermaid diagrams and contract usage](docs/table-and-observation-model.md) explain the staged design. Iceberg remains host/cloud work; the dictionary does not claim SensorThings API compliance.

An eight-row queue feeds a separate storage task and a bounded PSRAM batch. Default rotation is **15 minutes / up to 90 rows**, configurable to **10 minutes / up to 60 rows** for the running session. The writer emits immutable Parquet without an Arrow runtime, with **UNCOMPRESSED** and opt-in **LZ4_RAW** codecs. Reboot restores uncompressed output. Persistent station identity and UTC-aligned Hive partitions use:

```text
output/station=<UUID>/year=YYYY/month=MM/day=DD/
  data_HHMM_<boot>_<first>-<last>-<attempt>.parquet
```

Measured on one board/card: a full automatic 60-row batch was **28,059 bytes** and finalized in **122 ms**, with no recorded drops or missed deadlines. A later full **90-row uncompressed Hive file was 39,869 bytes** and also passed both readers with zero recorded health errors. See the dated [bench record](docs/bench-verified.md) for image identities and scope, and the [compression experiment](docs/compression-benchmark.md) for codec comparisons.

**What the evidence establishes:** direct on-device Parquet is feasible for this workload and its files interoperate with PyArrow/DuckDB without host conversion. On the same 60 rows, three LZ4 comparisons produced **14,080-byte files versus 28,537 bytes uncompressed (50.7% smaller)** and **93.0 ms versus 118.4 ms median finalization (21.4% faster)**. That supports choosing LZ4 for this tested logger; it is not a claim of superiority over every format, card, codec or cloud architecture. Filesystem allocated space, energy use and production durability have not been compared.

Offline logging needs no internet. The measured 60-row LZ4 rate projects to about **2.03 MB/day / 0.74 GB/year before filesystem overhead** at ten-minute rotation. A 32 GB card therefore offers multi-year storage capacity in principle, not a guaranteed card or battery lifetime. [Capacity assumptions and future synchronization](docs/telemetry-pipeline.md#offline-capacity-and-reconnection) explain allocation-unit overhead, power, clock drift and the uploader that still needs to be built.

**Feasibility, not production durability:** unfinished RAM rows are lost on reset; partial files are retained but not repaired. Power-cut recovery, upload, cloud compaction and Iceberg remain open; Snappy/Zstd results are host-only. UTC is a host-supplied estimate and must be supplied after each reboot; until then, files go under the station's `unsynced/boot=<boot>/` tree with null UTC values. Unsupported battery current and ambient temperature/humidity are null; camera/audio streams are outside this scalar trial.

## Getting started

```sh
pixi install
pixi run ports     # find the board
pixi run chip      # confirm it is an ESP32-S3, read-only
pixi run flash-id  # confirm 16 MB flash
pixi run efuse     # read-only security and flash-type check
pixi run backup    # full flash image before the first write
ls -l backup/     # confirm the image is exactly 16777216 bytes
```

Read the "Do not brick the board" section of `AGENTS.md` before flashing anything.

Build the active firmware with `pixi run arduino-setup` and `pixi run arduino-build`. Flash only after the safety sequence, using an explicit checked port:

```sh
pixi run arduino-flash /dev/cu.usbmodem101
```

Then supply time and inspect the logger, using the checked port and only one serial process at a time:

```sh
pixi run parquet-device sync-time --port /dev/cu.usbmodem101
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet status'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet list'
pixi run parquet-test --sanitize
```

The [trial README](firmware/arduino-m5unified/README.md#inspect-the-live-logger) has bounded capture, flush, fetch and DuckDB query commands. Prefer its Parquet serial helper during logging: serial control-line settings caused unwanted resets in the initial host implementation and were corrected for this macOS/CoreS3 pair. Do not reset merely to read data.

Keep exports and captures in the trial's git-ignored **`artifacts/`**, never `build/`: Arduino rebuilds can clean their build directory. `pixi run python tools/export_parquet.py --port <port> --out firmware/arduino-m5unified/artifacts/exports/<new-name>` retrieves every listed finalized file, including legacy files on the current firmware, without deleting or flushing device data.

Firmware SDKs are not conda packages, so the project fetches them itself at pinned versions. A clean machine needs `pixi install` and then the setup task for whichever trial you are building, with no manual SDK installation. SDKs land in `$M5_TOOLCHAIN_ROOT`, default `~/.cache/m5stack-aq-parquet/toolchains`, deliberately outside the repo so git worktrees share one copy. See `docs/cores3-development.md`.

## Hardware

M5Stack CoreS3 (K128), 16 MB flash, 8 MB Quad PSRAM. The PM2.5 air-quality module (M134 / PMSA003) is an optional add-on and is not assumed to be attached.

## License and attribution

Licensed under [CC BY 4.0](LICENSE), matching other Walkthru.Earth repositories.

Exception: vendored [LZ4 1.10.0](firmware/arduino-m5unified/vendor/lz4/README.md) retains its BSD-2-Clause license and upstream notices.

**If you use any of this work, you must credit us visibly.** Attribution belongs
somewhere a reader actually sees it, such as your README, your documentation, your
about screen, or your paper. A buried comment in source does not count.

Minimum credit.

> Based on [walkthru-earth/m5stack-aq-parquet](https://github.com/walkthru-earth/m5stack-aq-parquet)
> by Walkthru.Earth, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

BibTeX.

```bibtex
@software{walkthru_m5stack_aq_parquet,
  author  = {Youssef Harby, Myagmarjargal Mendbayar},
  title   = {m5stack-aq-parquet: Air-quality logging firmware for M5Stack CoreS3},
  year    = {2026},
  url     = {https://github.com/walkthru-earth/m5stack-aq-parquet},
  license = {CC-BY-4.0},
  note    = {Walkthru.Earth}
}
```

State what you changed if you modified the work. Attribution does not imply we endorse your project.
