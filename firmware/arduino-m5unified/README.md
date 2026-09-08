# Trial, Arduino-ESP32 with M5Unified

**Status: active. Real-sensor Parquet SD logging, automatic 60-row batching and UTC Hive partition/readback checks were verified on hardware on 2026-09-08.**

This is the only active framework trial. It establishes the CoreS3 hardware baseline and tests whether a bounded C++ writer can produce interoperable Parquet from real ten-second measurements on microSD. It owns its toolchain bootstrap, exact dependency versions, board options, partition layout and build outputs. Networking and Iceberg are deferred.

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
6. microSD mount on the display-shared SPI bus, without formatting;
7. PMSA003 framing, length and checksum on GPIO18 RX and GPIO17 TX.

After bring-up, the UART parser continues draining the active PMS stream. Every 10 seconds the LCD and serial output refresh from the latest checksum-valid frame. The LCD has three pages: atmospheric and CF=1 PM1.0/PM2.5/PM10 mass values; all six particle-count thresholds with sensor and parser integrity; and live SD, power, heap, PSRAM and uptime health. Swipe vertically or tap to move between pages. Page changes redraw immediately without changing the measurement cadence.

The scan excludes reserved I2C addresses `0x00` through `0x07`. Probing that range stopped the ESP32-S3 controller during the first hardware run; M5Unified's own scanner carries the same restriction.

## Verified result

The first diagnostic confirmed 16 MB QIO flash, **8 MB Quad PSRAM**, all nine expected onboard I2C devices, working IMU and touch communication, a mounted nominal 32 GB SDHC card and a checksum-valid PMSA003 frame. The RTC acknowledged at `0x51` but its date/time read failed. PMIC values indicated USB input and near-zero battery voltage, so battery presence and charging remain unverified.

The full values and test method are recorded in [the bench record](../../docs/bench-verified.md). The initial verified sketch used 551,263 bytes of program storage and 26,188 bytes of static internal RAM. The single-page live-display revision used 552,795 bytes and 26,268 bytes; the pre-Parquet three-page touch revision used 554,315 bytes and 26,276 bytes. That image was flashed with hash verification, produced a fresh 10-second sensor report with zero parser failures and logged page changes from touch gestures. The current Hive logger uses 579,711 bytes of program storage and 26,988 bytes of static internal RAM. The physical layout and gesture feel still need a deliberate visual check by someone looking at the board.

## On-device Parquet feasibility

`telemetry_logger.cpp` takes one snapshot every **10 seconds**, using a monotonic deadline rather than adding processing time to the interval. Default rotation is **900 seconds / up to 90 rows**; **600 seconds / up to 60 rows** is selectable over serial. With UTC supplied, files follow aligned UTC windows (`:00/:15/:30/:45` by default); the initial or manually flushed file can be shorter. A sample is a snapshot, not a ten-second average. Missed deadlines advance the sequence and increment a counter instead of manufacturing historical readings.

The 73-column schema includes device/boot identity and sequence; monotonic time, schedule, jitter and clock epoch; both PMS mass-concentration variants and all six particle-count channels; framing/status/age; acceleration and angular velocity; auxiliary magnetometer raw counts; IMU die temperature; LTR-553 raw light channels and proximity; power reports; RTC calendar fields; touch; memory and storage health. Names carry numeric units. The code's field list is the schema source of truth. The initial flat-path bench image had 72 columns, before adding the clock epoch.

PMS measurements are null during the initial 30-second warm-up, after five seconds without a valid frame, or when the sensor reports an error. Status and parser counters remain available. Magnetic values are uncalibrated raw counts, light/proximity are raw ADC counts rather than lux/distance, and IMU temperature is **not ambient temperature**. RTC reads currently fail. UTC stays null until the explicit `sync-time` command supplies the host's current clock; subsequent rows carry `clock_status=1` (host estimate), while old rows remain unchanged. Each time update increments `clock_epoch` and separates batches. Host time is not a calibrated synchronization/uncertainty guarantee. Ambient temperature/humidity remain null because SHT20's electrical isolation is unresolved. CoreS3 battery current is unsupported by M5Unified and stays null; voltage/percentage/charging are PMIC reports, not proof of battery presence. Camera and audio streams are not acquired in this scalar telemetry trial.

Acquisition and M5 board-service reads run in the Arduino loop. An eight-record queue feeds a separate FreeRTOS storage task; a 90-row batch, column descriptors and writer workspace use about **62 KiB of PSRAM** (the exact allocation is printed at startup). This allows the scheduler to use both cores without explicit task pinning. One application mutex serializes SD and display access, including completion of display DMA. Sensor code never writes SD. The storage task has a 16 KiB stack and uses 4 KiB internal stdio staging; the format writer itself needs 2 KiB caller workspace and no dynamic allocation.

The writer supports flat nullable/required INT32, INT64 and FLOAT, PLAIN encoding, RLE definition levels, Data Page V1 and **UNCOMPRESSED** pages. One row group and one page per column are written sequentially, followed by Thrift Compact metadata and the footer. It is a deliberately limited format writer, not Apache Arrow or a general Parquet implementation. Compression is the next benchmark, not an implemented feature.

Files use **`/output/` on the card** (`/sd/output` in firmware). A station UUID is generated once and saved in the `parquet` NVS namespace; normal resets/flashes preserve it. Date partitions use UTC, never local time or the unreadable RTC:

```text
output/
└── station=<persistent-UUID>/
    └── year=2026/month=09/day=08/
        ├── data_0900_<boot>_<first>-<last>-<attempt>.parquet
        └── data_0915_<boot>_<first>-<last>-<attempt>.parquet
```

The suffix prevents restarts, clock corrections and forced flushes from overwriting a file in the same window. Before UTC is supplied, files instead use `station=<UUID>/unsynced/boot=<boot>/data_unsynced_...parquet`. Existing dated and unsynced files are never relocated when the clock changes. The legacy first-run files under `/parquet/` are also retained on the card; the current `list` command enumerates `/output/` only.

The worker creates files exclusively, writes `.partial`, checks all writes, calls `fflush` and `fsync`, closes, verifies structural completion and reads CRC32, then renames to `.parquet`. Files are retained; no automatic deletion or upload exists. On write failure the batch is retained in RAM, later rows are counted as dropped, and `parquet flush` can retry. Existing partial files are reported and retained on boot; no repair or deletion is attempted.

**Durability limit:** unfinished RAM samples are lost on reset, potentially the full configured batch. FAT rename and SD controller persistence have no power-loss guarantee here. The structural completion check is not a full Parquet validator; host readers perform that check. A recovery spool, power-cut tests, full-card/removal handling and sustained radio/display stress remain later work.

### Inspect the live logger

Only one host process may own the port at a time. Identify its current path first.

```sh
pixi run parquet-test --sanitize
pixi run parquet-device sync-time --port /dev/cu.usbmodem101
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet status'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet list'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet flush'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet interval 900'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet interval 600'
pixi run parquet-device capture --port /dev/cu.usbmodem101 --seconds 650 --until-ready --out firmware/arduino-m5unified/build/parquet-capture.log
pixi run parquet-device fetch --port /dev/cu.usbmodem101 '<reported-relative-path>.parquet' --out firmware/arduino-m5unified/build/output
pixi run parquet-device bench --port /dev/cu.usbmodem101 --sync-time --seconds 960 --until-ready --out firmware/arduino-m5unified/build/output --log firmware/arduino-m5unified/build/hive-bench.log
```

Changing the interval first flushes any current rows; the setting lasts until reboot, when it returns to 900 seconds. UTC must be supplied again after reboot until a persistent trusted clock or network time source is implemented. `fetch` reads the **already-written SD file** in bounded chunks over USB, verifies length/CRC32, compares every stored value/null through PyArrow and DuckDB, and preserves the Hive directories beneath the chosen local output directory. There is no host-side format conversion. Artifacts under `build/` are git-ignored. `bench` captures and fetches through one serial connection; use a fresh log path, or omit `--log`.

`--until-ready` stops at the first finalized file, which can be a short initial window or old unsynced batch split by `--sync-time`; it does not assert 90 rows. Check the reported row count and time range before claiming full-window coverage. For a quick current-schema smoke test, use a fresh log name:

```sh
pixi run parquet-device bench --port /dev/cu.usbmodem101 --sync-time --seconds 35 --flush-after-capture --out firmware/arduino-m5unified/build/output --log firmware/arduino-m5unified/build/hive-smoke.log
```

The smoke command changes the clock anchor and manually finalizes a short batch; it is not an endurance test. `flush` does not stop sampling or unmount SD, so it is not a safe-eject command. `status`, `list` and `fetch` need no device reset. The host fetcher rejects unsafe relative paths and differing existing destinations, while accepting an identical already-verified file; it preserves both device files and local Hive structure.

For querying dated files, use DuckDB's Hive discovery explicitly; the `station`, `year`, `month` and `day` fields come from the path:

```sql
SELECT station, year, month, day, sequence, pm25_atmospheric_ug_m3
FROM read_parquet('output/station=*/year=*/month=*/day=*/*.parquet',
                  hive_partitioning=true);
```

The host validator disables Hive inference when comparing stored file columns; query-time partition discovery is a separate check.

On this macOS/CoreS3 pair, opening serial with both DTR and RTS deasserted caused an unwanted USB reset. The Parquet helper keeps both asserted and clears POSIX HUPCL; separate status, flush and fetch operations preserved the boot and buffered sequence during the bench test. Other adapters/hosts need their own check.

The earlier 72-column image produced a full automatic **60-row, 28,059-byte** batch that matched in PyArrow 25.0.0 and DuckDB 1.5.5. Finalization took **122,041 µs**, including 13,317 µs in flush/sync/close; no drops, missed deadlines or storage errors were recorded. The current 73-column image passed short-file readback, UTC Hive partition discovery, a quarter-hour boundary split, persistent station identity and byte-for-byte finalized-file retention across a normal restart. Its initial 3-row Hive file was 8,319 bytes and finalized in 58,463 µs. These are individual observations, not latency bounds. A full 90-row/15-minute hardware run and power-cut tests remain open. See [bench results](../../docs/bench-verified.md) for artifacts, hashes and exact scope.

The contract and later cloud design remain in [the telemetry pipeline](../../docs/telemetry-pipeline.md). A second framework trial requires a concrete limitation from this one, recorded here first.

## Recovery

The original 16 MB UIFlow image is preserved under `backup/`. `pixi run restore backup/<file>.bin` writes it back and is destructive, so name the image explicitly. Never write eFuses or raise the esptool baud on this board.

**Last verified on hardware: 2026-09-08, board MAC ending `6b:40`.**
