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

## Board 1, bring-up firmware

Verified **2026-09-08** with `cores3-bringup-v1`, built from Arduino-ESP32 3.3.11, M5Unified 0.2.21 and M5GFX 0.2.28. The image was flashed at 115200 after repeating the read-only identity, flash and eFuse checks and confirming the existing 16 MB backup. Serial output was captured after a physical RST press; the helper reconnected after macOS re-enumerated USB.

| Property | Measured value | How |
| --- | --- | --- |
| Runtime board | M5Stack CoreS3, ESP32-S3 revision 2, two cores | Diagnostic chip report |
| Flash at runtime | 16,777,216 bytes, QIO, 80 MHz | Arduino `ESP` runtime APIs |
| PSRAM at runtime | **8,388,608 bytes detected**; 8,385,136 free and 8,257,524 largest free block after board initialization | Arduino `ESP` and IDF heap-capability APIs |
| Internal heap | 380,688 bytes total, 335,268 free, 329,996 minimum free and 278,516 largest free block | Arduino `ESP` and IDF heap-capability APIs |
| Internal I2C | Nine expected devices, no unexpected devices: `0x21`, `0x23`, `0x34`, `0x36`, `0x38`, `0x40`, `0x51`, `0x58`, `0x69` | M5Unified-owned internal bus at 100 kHz |
| PMIC and USB | Type 4; external 5 V on; USB in input mode; VBUS 5,134 mV | M5Unified `M5.Power` APIs |
| Battery report | 27 mV, 0 percent, discharging and 0 mA | M5Unified `M5.Power` APIs; this does not establish whether a battery is attached |
| RTC | Device acknowledged at `0x51`; the date/time read failed | M5Unified I2C and RTC APIs |
| IMU | Enabled as type 6; accelerometer and gyroscope reads succeeded | M5Unified IMU API |
| Touch | Controller acknowledged at `0x38`; zero active points during the capture | M5Unified I2C and touch APIs |
| microSD | One nominal 32 GB SDHC card mounted at 25 MHz; card size 31,457,280,000 bytes, filesystem size 31,441,764,352 bytes | Arduino SD API; no format or file write |
| PMSA003 | Checksum-valid frame, sensor error 0, firmware 151 | GPIO18 RX/GPIO17 TX at 9600, bounded five-second parser |
| Live PMS interval | Two successive reports approximately 10 seconds apart; latest-frame ages 423 ms and 804 ms; zero checksum and length failures across 23 frames | 24-second bounded serial capture after flashing the live-display revision |
| Touch UI | Final image uses 554,315 bytes of program storage and 26,276 bytes of static internal RAM. Flash hashes verified. Up and down swipes plus taps reached all three pages while four timed PMS reports continued with zero parser failures. | Flash plus a 40-second serial capture during the touch test |

The PMS frame reported atmospheric PM1, PM2.5 and PM10 values of 19, 24 and 26 µg/m³. This confirms framing and transport for the attached sensor; one reading does not validate calibration.

## Board 1, on-device Parquet and Hive partitions

Verified **2026-09-08**, on the same board and mounted nominal 32 GB SDHC card, using the active Arduino trial and the pinned versions above. Repeated port, chip, flash-ID and read-only eFuse checks before flashing; confirmed `backup/cores3-flash-20260906T102150Z.bin` is exactly 16,777,216 bytes. Flash hashes verified. No formatting, file deletion, eFuse write or radio setup was performed.

The firmware collected real scalar snapshots every ten seconds while the PMS parser and display continued running. A separate storage task wrote Parquet directly to SD; the host fetched the existing bytes over USB, checked transfer length/CRC32 and compared every stored value/null with **PyArrow 25.0.0 and DuckDB 1.5.5**. This was not host-side conversion. All files below use uncompressed pages, one row group and one page per column.

| Run | Measured result |
| --- | --- |
| Initial manual flush, 72-column flat-path image | 7 rows, sequences 0–6; 9,494 bytes, including a 5,490-byte footer; finalization 51,998 µs, flush/sync/close 12,765 µs |
| Automatic 10-minute batch, same image | **60 rows**, sequences 7–66; **28,059 bytes**; finalization **122,041 µs**, flush/sync/close **13,317 µs**; both readers matched |
| Sampling in the 60-row file | Adjacent intervals 9,990,877–10,015,182 µs; mean 10,000,237 µs; maximum deadline jitter 18,671 µs. Drop, missed-deadline, storage-error and PMS checksum/length-error counters were all zero in the file |
| Memory at that automatic finalization | Internal heap free 310,216 bytes; PSRAM free 8,290,860 bytes; reported storage-task stack free 7,480 bytes |
| Current Hive image | **73 columns**, including `clock_epoch`; program storage 579,711 bytes, static internal RAM 26,988 bytes; PSRAM batch/descriptors/workspace allocation 63,560 bytes, 664 bytes per in-memory row; default interval 900 seconds |
| UTC Hive file, manual flush | 3 rows, sequences 0–2; 8,319 bytes; finalization 58,463 µs, flush/sync/close 11,298 µs; all UTC values present with host-clock status/epoch 1. UTC date and 18:30 window agreed with the path. Both readers matched; DuckDB Hive discovery returned the expected station and date |
| Automatic quarter-hour boundary | The next file contained sequence 3 under `data_1830_…` (7,630 bytes, both readers matched); subsequent sequences 4–9 were finalized under `data_1845_…`. This verifies a boundary split, **not a full 90-row/15-minute run** |
| Hive image memory at initial finalization | Internal heap free 308,260 bytes; PSRAM free 8,288,812 bytes; reported storage-task stack free 5,632 bytes |
| Normal software restart | Station UUID remained `53315f5f-cb85-4d8d-b623-d56266084189`; boot ID changed. Three finalized dated files were still enumerated. Re-fetching the initial 8,319-byte Hive file matched the pre-restart file byte-for-byte and passed both readers again |
| No UTC after restart | A new 3-row, 8,295-byte file was written under `station=<UUID>/unsynced/boot=<new-boot>/`; all three UTC values were null and both readers matched. Unsynced rows were not assigned a fabricated calendar date |
| Host time restored after restart | The pending unsynced sequence 22 was finalized separately; sequences 23–25 formed a dated 8,343-byte `data_1845_…` file, CRC32 `7d898d53`. Both readers and per-row UTC/path agreement passed. Capture: `hive-resync.log` |
| Runtime interval commands | `parquet interval 600` and then `parquet interval 900` each finalized pending rows and acknowledged the new interval. The device was returned to 900 seconds |
| Final live status | `interval_s=900 buffered=11 finalized=5 dropped=0 errors=0 queue_peak=1 failed=false`; host time had been restored and the logger was left running |
| Scalar sensor availability | PMS values became non-null after the software warm-up gate; IMU freshness mask was 7; raw light and proximity reads were valid. RTC calendar, ambient temperature/humidity and unsupported battery current remained null. This verifies acquisition, not sensor calibration or warm-up sufficiency |

`write_us` measures the finalization path including writing, sync/close, structural readback, CRC and rename; `sync_us` is its flush/sync/close subset. These are individual observations, not latency bounds or p99 estimates. Free-stack values are runtime reports, not a qualified worst-case margin. The workers were not explicitly pinned to cores; no dual-core speedup was measured.

Local artifacts live under the trial's git-ignored `build/` directory. They are not included in a Git clone; the hashes below identify the measured evidence, not a remotely published artifact bundle. Card brand/model and accessory PCB revision/population were not recorded, so do not extrapolate these results to all cards or M134 revisions:

- `parquet-readback/f1179d54856fa20c31038836e5ec177e-7-66-1.parquet`: CRC32 `785aa6d0`; SHA-256 `317080e640485265258462bae724d8095916368d19a66c6919e18c86c213d4e3`. `parquet-ten-minute.log` contains 56 row reports whose sequence/monotonic timestamps match their stored rows; independently refreshed display measurements were not used as exact-value comparisons.
- `output/station=53315f5f-cb85-4d8d-b623-d56266084189/year=2026/month=09/day=08/data_1830_c838cf99f4a1e867ebec9e2752bad519_0-2-0.parquet`: CRC32 `ce4bb0b8`; SHA-256 `ad0b32e81b233a68a867e31df8db45222dacf4a137ea3a244390389ee53bfd51`. Capture: `hive-first.log`.
- Restart capture/readback: `hive-restart.log`, boot `1491409bed013e1999dc4072ac91ab0f`, unsynced-file CRC32 `25a3b7f7`.
- Current flashed application binary SHA-256: `5227a989df0a9b3264117e773d3bc8a54c235a2f83d1e54037ab8966fab2c4b9`. Earlier 72-column application SHA-256: `9d5684465ec639c59fac19fa72ccb37c5429b97f22334eba6a29ee08f00a303f`.

Separately, **host-only** `pixi run parquet-test --sanitize` passed empty, one-row, 90-row × 96-column and 65,536-row fixtures, including exact integer boundaries, nulls and IEEE float edge cases, with both readers and address/undefined-behavior sanitizers. These tests do not establish device performance or SD failure behavior.

## Measured facts that changed how we work

- **Do not force a high baud on this board.** Reading the full 16 MB at the esptool default succeeded in **97 seconds**, about 1382 kbit/s. The same read with `--baud 921600` aborted at roughly 1.8 percent with `Serial data stream stopped, possible serial noise or corruption`, and wrote no file. The transport here is native USB-Serial/JTAG, so the requested baud buys nothing and costs reliability. The `backup` and `restore` tasks therefore pass no `--baud`.
- **A failed backup can look like success.** The first attempt exited zero because the failure was hidden behind a shell pipeline, while esptool had actually aborted and produced no file. Always confirm `backup/` really contains a 16777216-byte file before flashing anything.

- **A board in download mode is silent, and that is normal.** Held in the ROM download bootloader the board enumerated fine, answered `esptool chip-id` and `flash-id` every time, yet returned zero bytes on the CDC port across two attempts, including after a DTR and RTS reset pulse and a REPL interrupt. No application is running there, so there is nothing to print. Do not read that silence as a failed board. `pixi run capture` reports this case explicitly rather than hanging.
- **Physical RST re-enumerates USB.** macOS removed and recreated the serial device after a short press. The bounded capture helper now reconnects during its timeout.
- **Serial open can reset this native-USB board.** The initial Parquet host helper deasserted both DTR and RTS and caused an unwanted reset on each open, losing buffered rows. Keeping both asserted and clearing POSIX HUPCL preserved boot identity and buffered sequences across separate status, flush and fetch operations on this macOS/CoreS3 pair. Other hosts/adapters remain unqualified.
- **Do not scan reserved I2C addresses on ESP32-S3.** Probing `0x01` stopped the first diagnostic. Restricting the scan to `0x08` through `0x77`, as M5Unified itself does, completed and found all nine expected devices.
- **A blank core-dump partition logs one checksum error.** The first boot after flashing reported an expected stored checksum of `0xffffffff` from the unused partition; the application then completed normally. Keep crash diagnostics, but distinguish an empty partition from a new panic in log ingestion.
- **`PIN_POWER_SELECTION` reads `VDD3P3_CPU`.** That is the expected setting for GPIO33 to GPIO37 on a Quad-memory board, and it is consistent with those pins being available to the onboard LCD and microSD circuits rather than consumed by Octal PSRAM.
- **A first SDK bootstrap costs tens of minutes.** About 1.3 GB downloaded in roughly 20 minutes without either an Arduino or an ESP-IDF toolchain finishing. Run one bootstrap at a time, and do not move the cache mid-download, which restarts transfers already in flight. Sizes are recorded in `firmware/arduino-m5unified/README.md`.

## PSRAM eFuse and runtime results

The eFuse block reports `PSRAM_CAP = None`, `PSRAM_VENDOR = None` and the derived `PSRAM_CAPACITY = 0`. **This is not evidence that the board lacks PSRAM.** Those fuses describe PSRAM packaged inside the ESP32-S3 module, and CoreS3 carries its 8 MB Quad PSRAM as a separate part. The only sound check is at runtime from firmware, by reading the detected SPIRAM size.

The diagnostic detected exactly **8,388,608 bytes**, so the separate 8 MB PSRAM is now bench-verified. The build configuration selected QSPI/Quad PSRAM and explicitly did not select Octal. A future zero reading should be treated as a build or initialization regression.

## Still unverified on hardware

Nothing below has been observed on a real board yet. Do not promote any of it into this file without a measurement.

- microSD power-cut recovery, injected short-write failures, card removal/full-media behavior and long-running radio/display stress. Small-batch writes, sync and readback are verified above; normal-reset retention is not a power-loss guarantee.
- A complete automatic 90-row/15-minute batch on the current Hive image, midnight rollover and arbitrary clock corrections. Host time accuracy/drift, compression and object-storage upload.
- Battery presence, charging behavior and current measurement with a known battery state.
- RTC date/time validity and retention.
- LCD page text and layout for clipping under explicit visual inspection.
- Long-duration PMSA003 sampling, physical warm-up sufficiency and optional SHT20 isolation; the ten-minute acquisition run above is not lifetime qualification.
- Wi-Fi, BLE, upload, OTA, watchdog and sleep behavior.

Move a line out of this list only after a dated measurement records the method and result.
