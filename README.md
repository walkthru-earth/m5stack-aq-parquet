# m5stack-aq-parquet

Air-quality logging firmware for **M5Stack CoreS3 (ESP32-S3)**. The device stores recoverable measurement segments; cloud or host tooling converts committed data to Parquet.

The repo can hold **more than one framework trial** against the same board, sharing one set of hardware reference docs. It runs one trial at a time, and a second is opened only when a measured result justifies it.

- `AGENTS.md` is the entry point for humans and coding agents.
- `docs/` holds framework-neutral CoreS3 references, including the [telemetry and Parquet pipeline](docs/telemetry-pipeline.md).
- `firmware/<framework>-<variant>/` holds each self-contained trial.

## Current status

The only active trial is [Arduino-ESP32 with M5Unified](firmware/arduino-m5unified/README.md), pinned to Arduino-ESP32 3.3.11, M5Unified 0.2.21 and M5GFX 0.2.28. Its diagnostic has been built and verified on the real board. The current flashed image shows all PMSA003 mass and particle-count values on the LCD and refreshes from a checksum-valid frame every 10 seconds.

The storage design uses a bounded PSRAM queue, one microSD writer, recoverable immutable segments and idempotent upload. Current C/C++ Parquet libraries are assessed in the telemetry note; direct Parquet on the S3 remains a later conformance and power-cut experiment.

## Getting started

```sh
pixi install
pixi run ports     # find the board
pixi run chip      # confirm it is an ESP32-S3, read-only
pixi run backup    # full flash image before the first write
```

Read the "Do not brick the board" section of `AGENTS.md` before flashing anything.

Build the active firmware with `pixi run arduino-setup` and `pixi run arduino-build`. Flash only after the safety sequence, using an explicit checked port:

```sh
pixi run arduino-flash /dev/cu.usbmodem101
```

Firmware SDKs are not conda packages, so the project fetches them itself at pinned versions. A clean machine needs `pixi install` and then the setup task for whichever trial you are building, with no manual SDK installation. SDKs land in `$M5_TOOLCHAIN_ROOT`, default `~/.cache/m5stack-aq-parquet/toolchains`, deliberately outside the repo so git worktrees share one copy. See `docs/cores3-development.md`.

## Hardware

M5Stack CoreS3 (K128), 16 MB flash, 8 MB Quad PSRAM. The PM2.5 air-quality module (M134 / PMSA003) is an optional add-on and is not assumed to be attached.

## License and attribution

Licensed under [CC BY 4.0](LICENSE), matching other Walkthru.Earth repositories.

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
