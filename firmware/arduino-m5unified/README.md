# Trial, Arduino-ESP32 with M5Unified

**Status: active. The bring-up milestone was built, flashed and verified on real hardware on 2026-09-08.**

This is the only active framework trial. Its first job is to establish the CoreS3 hardware baseline before storage, networking or cloud code is added. It owns its toolchain bootstrap, exact dependency versions, board options, partition layout and build outputs.

## Pinned platform

| Dependency | Exact version |
| --- | --- |
| Arduino CLI | 1.5.1 |
| Arduino-ESP32 | 3.3.11, based on ESP-IDF 5.5.5 |
| M5Unified | 0.2.21 |
| M5GFX | 0.2.28 |
| Board FQBN | `esp32:esp32:m5stack_cores3` |

`dependencies.lock` is the source of truth. The FQBN explicitly selects 16 MB flash, QIO flash, **QSPI/Quad PSRAM**, hardware USB CDC and 115200 upload speed. `build.sh` checks the generated SDK configuration and fails if it finds Octal PSRAM, the wrong flash size or a non-custom partition table.

The custom 16 MB partition table has two 6 MB OTA application slots, NVS, OTA state, a core-dump partition and a remaining SPIFFS partition. Measurement data belongs on microSD; the flash filesystem is not the primary sample store.

## Commands

Run all commands from the repository root:

```sh
pixi install
pixi run arduino-setup
pixi run arduino-boards
pixi run arduino-build
pixi run arduino-flash /dev/cu.usbmodem101
pixi run capture --port /dev/cu.usbmodem101 --seconds 20 --until "DIAG COMPLETE"
```

The port is an example. Identify the connected board with `pixi run ports` and repeat the safety sequence in the root `AGENTS.md` before a first write to another board. Setup keeps downloaded SDKs and libraries below `$M5_TOOLCHAIN_ROOT`, which defaults outside the repository. The completed Arduino cache measured 7.5 GB on 2026-09-08; Arduino CLI adds 34 MB.

A physical RST press makes this CoreS3 disappear and re-enumerate on macOS. The capture helper reconnects to the same device path during its bounded timeout. A short press boots the application; holding RST for about three seconds enters the silent ROM downloader.

## Bring-up and live sensor firmware

`bringup/bringup.ino` emits a machine-readable `cores3-bringup-v1` report. It uses M5Unified as the sole owner of internal board services and checks:

1. chip, revision, MAC, reset cause, flash size, mode and speed;
2. internal heap and detected PSRAM;
3. documented internal I2C devices;
4. PMIC, VBUS and battery reporting through `M5.Power`;
5. RTC, IMU and touch;
6. microSD mount on the display-shared SPI bus, without formatting or writing;
7. PMSA003 framing, length and checksum on GPIO18 RX and GPIO17 TX.

After bring-up, the UART parser continues draining the active PMS stream. Every 10 seconds the LCD and serial output refresh from the latest checksum-valid frame. The LCD shows atmospheric and CF=1 PM1.0/PM2.5/PM10 mass values, all six particle-count thresholds, frame age, sensor status, parser failures and SD state.

The scan excludes reserved I2C addresses `0x00` through `0x07`. Probing that range stopped the ESP32-S3 controller during the first hardware run; M5Unified's own scanner carries the same restriction.

## Verified result

The first diagnostic confirmed 16 MB QIO flash, **8 MB Quad PSRAM**, all nine expected onboard I2C devices, working IMU and touch communication, a mounted nominal 32 GB SDHC card and a checksum-valid PMSA003 frame. The RTC acknowledged at `0x51` but its date/time read failed. PMIC values indicated USB input and near-zero battery voltage, so battery presence and charging remain unverified.

The full values and test method are recorded in [the bench record](../../docs/bench-verified.md). The initial verified sketch used 551,263 bytes of program storage and 26,188 bytes of static internal RAM. The live-display revision uses 552,795 bytes of program storage and 26,268 bytes of static internal RAM. It was flashed and produced two verified 10-second serial refreshes; the LCD layout still needs a visual check by someone looking at the board.

## Next milestone

Keep this trial active while implementing the store-and-forward measurement path:

1. timestamped, validity-aware sensor records in a bounded PSRAM queue;
2. one storage worker writing recoverable immutable segments to microSD;
3. boot recovery and explicit full-card/drop behavior;
4. measured write, sync and display-coexistence latency;
5. host conversion of committed segments to Parquet.

The contract, cloud pipeline and current C/C++ library research are in [the telemetry pipeline](../../docs/telemetry-pipeline.md). Direct Parquet on the microcontroller is a separate measured experiment after the durable segment path passes power-cut tests. A second framework trial requires a concrete limitation from this one, recorded here first.

## Recovery

The original 16 MB UIFlow image is preserved under `backup/`. `pixi run restore backup/<file>.bin` writes it back and is destructive, so name the image explicitly. Never write eFuses or raise the esptool baud on this board.

**Last verified on hardware: 2026-09-08, board MAC ending `6b:40`.**
