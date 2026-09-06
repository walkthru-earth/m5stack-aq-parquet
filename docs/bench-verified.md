# Bench-verified board record

[Router](README.md) · Everything else in `docs/` is source-checked against schematics and vendor code. This file records only what was **measured on real hardware in this project**, with the date and the method. If a claim elsewhere disagrees with this file, this file wins for the board listed here.

## Board 1, first unit

Verified **2026-09-06** over USB-C to the native USB-Serial/JTAG port, using `esptool` 5.3.1 from the pixi environment. No firmware of ours had been flashed yet, so these are read-only observations of a factory board.

| Property | Measured value | How |
| --- | --- | --- |
| Chip | ESP32-S3 (QFN56) revision **v0.2** | `pixi run chip` |
| Features | Wi-Fi, BT 5 LE, dual core plus LP core, 240 MHz | `pixi run chip` |
| Crystal | 40 MHz | `pixi run chip` |
| Base MAC | `44:1b:f6:e2:6b:40` | `pixi run chip` |
| USB mode | USB-Serial/JTAG | `pixi run chip` |
| Flash | 16 MB, manufacturer `0x46`, device `0x4018` | `pixi run flash-id` |
| Flash lines | Quad, 4 data lines, confirmed in eFuse `FLASH_TYPE` | `pixi run efuse` |
| Flash voltage | 3.3 V, set by strapping pin | `pixi run flash-id` |
| Wafer | major 0, minor 2, `PKG_VERSION` 0 | `pixi run efuse` |
| GPIO33 to GPIO37 supply | `PIN_POWER_SELECTION` is `VDD3P3_CPU` | `pixi run efuse` |
| Security fuses | Secure boot, flash encryption and JTAG disable are all **unburned**. The board is fully recoverable. | `pixi run efuse` |

Full 16 MB backup taken and kept before any write. The board shipped with a UIFlow MicroPython image, whose partition layout is recorded below for reference.

| Partition | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| nvs | data | nvs | `0x009000` | 24K |
| phy_init | data | phy | `0x00f000` | 4K |
| factory | app | factory | `0x010000` | 9792K |
| sys | data | fat | `0x9a0000` | 1024K |
| vfs | data | fat | `0xaa0000` | 4864K |
| storage | data | spiffs | `0xf60000` | 640K |

Flashing any trial in this repo overwrites that image. Inspect a saved backup with `pixi run backup-partitions backup/<file>.bin`.

## Measured facts that changed how we work

- **Do not force a high baud on this board.** Reading the full 16 MB at the esptool default succeeded in **97 seconds**, about 1382 kbit/s. The same read with `--baud 921600` aborted at roughly 1.8 percent with `Serial data stream stopped, possible serial noise or corruption`, and wrote no file. The transport here is native USB-Serial/JTAG, so the requested baud buys nothing and costs reliability. The `backup` and `restore` tasks therefore pass no `--baud`.
- **A failed backup can look like success.** The first attempt exited zero because the failure was hidden behind a shell pipeline, while esptool had actually aborted and produced no file. Always confirm `backup/` really contains a 16777216-byte file before flashing anything.

## PSRAM cannot be confirmed from eFuse on this board

The eFuse block reports `PSRAM_CAP = None`, `PSRAM_VENDOR = None` and the derived `PSRAM_CAPACITY = 0`. **This is not evidence that the board lacks PSRAM.** Those fuses describe PSRAM packaged inside the ESP32-S3 module, and CoreS3 carries its 8 MB Quad PSRAM as a separate part. The only sound check is at runtime from firmware, by reading the detected SPIRAM size.

So the documented "8 MB Quad PSRAM" remains **source-checked but not yet bench-verified**. The first diagnostic firmware to run must report detected PSRAM size, and the result belongs in this file. Until then, treat a zero PSRAM reading in firmware as a build-configuration bug, most likely Octal selected where Quad is correct, rather than as a hardware fact.

## Still unverified on hardware

Nothing below has been observed on a real board yet. Do not promote any of it into this file without a measurement.

- PSRAM presence, size and Quad mode at runtime.
- Every I2C address in the internal-bus table, and whether an unexpected device answers.
- microSD detection, mount, capacity, and the shared-bus GPIO35 handover between LCD D/C and SD MISO.
- AXP2101 rail states and battery reporting.
- Whether the optional M134 air-quality module is attached, and any PMSA003 frame.
- RTC, IMU, touch and display behavior.
