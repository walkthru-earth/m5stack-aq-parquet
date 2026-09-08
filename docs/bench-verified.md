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

The PMS frame reported atmospheric PM1, PM2.5 and PM10 values of 19, 24 and 26 µg/m³. This confirms framing and transport for the attached sensor; one reading does not validate calibration.

## Measured facts that changed how we work

- **Do not force a high baud on this board.** Reading the full 16 MB at the esptool default succeeded in **97 seconds**, about 1382 kbit/s. The same read with `--baud 921600` aborted at roughly 1.8 percent with `Serial data stream stopped, possible serial noise or corruption`, and wrote no file. The transport here is native USB-Serial/JTAG, so the requested baud buys nothing and costs reliability. The `backup` and `restore` tasks therefore pass no `--baud`.
- **A failed backup can look like success.** The first attempt exited zero because the failure was hidden behind a shell pipeline, while esptool had actually aborted and produced no file. Always confirm `backup/` really contains a 16777216-byte file before flashing anything.

- **A board in download mode is silent, and that is normal.** Held in the ROM download bootloader the board enumerated fine, answered `esptool chip-id` and `flash-id` every time, yet returned zero bytes on the CDC port across two attempts, including after a DTR and RTS reset pulse and a REPL interrupt. No application is running there, so there is nothing to print. Do not read that silence as a failed board. `pixi run capture` reports this case explicitly rather than hanging.
- **Physical RST re-enumerates USB.** macOS removed and recreated the serial device after a short press. The bounded capture helper now reconnects during its timeout.
- **Do not scan reserved I2C addresses on ESP32-S3.** Probing `0x01` stopped the first diagnostic. Restricting the scan to `0x08` through `0x77`, as M5Unified itself does, completed and found all nine expected devices.
- **A blank core-dump partition logs one checksum error.** The first boot after flashing reported an expected stored checksum of `0xffffffff` from the unused partition; the application then completed normally. Keep crash diagnostics, but distinguish an empty partition from a new panic in log ingestion.
- **`PIN_POWER_SELECTION` reads `VDD3P3_CPU`.** That is the expected setting for GPIO33 to GPIO37 on a Quad-memory board, and it is consistent with those pins being available to the onboard LCD and microSD circuits rather than consumed by Octal PSRAM.
- **A first SDK bootstrap costs tens of minutes.** About 1.3 GB downloaded in roughly 20 minutes without either an Arduino or an ESP-IDF toolchain finishing. Run one bootstrap at a time, and do not move the cache mid-download, which restarts transfers already in flight. Sizes are recorded in `firmware/arduino-m5unified/README.md`.

## PSRAM eFuse and runtime results

The eFuse block reports `PSRAM_CAP = None`, `PSRAM_VENDOR = None` and the derived `PSRAM_CAPACITY = 0`. **This is not evidence that the board lacks PSRAM.** Those fuses describe PSRAM packaged inside the ESP32-S3 module, and CoreS3 carries its 8 MB Quad PSRAM as a separate part. The only sound check is at runtime from firmware, by reading the detected SPIRAM size.

The diagnostic detected exactly **8,388,608 bytes**, so the separate 8 MB PSRAM is now bench-verified. The build configuration selected QSPI/Quad PSRAM and explicitly did not select Octal. A future zero reading should be treated as a build or initialization regression.

## Still unverified on hardware

Nothing below has been observed on a real board yet. Do not promote any of it into this file without a measurement.

- microSD write, sync, latency, power-loss recovery and long-running coexistence with display traffic.
- Battery presence, charging behavior and current measurement with a known battery state.
- RTC date/time validity and retention.
- LCD live-page appearance and touch coordinates under active input.
- Sustained PMSA003 sampling, warm-up behavior and optional SHT20 isolation.
- Wi-Fi, BLE, upload, OTA, watchdog and sleep behavior.

Move a line out of this list only after a dated measurement records the method and result.
