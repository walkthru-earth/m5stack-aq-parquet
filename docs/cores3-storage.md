# CoreS3 microSD and logging

[Router](README.md) · Read for the **built-in slot**, shared SPI, file logging or data export. Card presence is optional. Source snapshot **2026-09-06**; no card/firmware combination has been bench-tested here.

## Wiring and ownership

| Resource | CoreS3-specific requirement |
| --- | --- |
| Slot interface | **SPI**: SCK36, MISO35, MOSI37, CS4. Use Arduino `SD` or IDF SDSPI; the slot is not wired for native 4-bit `SD_MMC`. [M5 example][m5-sd] |
| Display sharing | LCD CS3; SCK36/MOSI37 shared. **GPIO35 is LCD DC while CS3 is low, and SD MISO while CS3 is high**. M5GFX switches its direction; a generic always-output LCD DC driver causes contention. [CoreS3 panel implementation][gfx] |
| SPI host | M5GFX's CoreS3 configuration uses `SPI2_HOST`; do not drive the same wires through another host or independently reinitialize its peripheral state. [Tagged configuration][gfx] |
| Power / detect | AXP2101 **ALDO4 = 3.3 V** powers the card; `TF_SW` is AW9523 **P0_4**, not an ESP GPIO. Board power/expander setup is required; see [hardware](cores3-hardware.md). [Power setup][power] |
| Capacity | M5 lists **16 GB maximum**; treat that as its documented support envelope. Larger cards require explicit board/card/filesystem qualification, not an assumed silicon limit. [Board specification][board] |

## Bring-up paths

- Arduino with M5Unified: follow [M5's current SD example][m5-sd]. Essential sequence: `M5.begin()` → `SPI.begin(36, 35, 37, 4)` → `SD.begin(4, SPI, 25000000, "/sd", 5, false)`; check every result. Last argument keeps automatic formatting off. This is a sequential bring-up recipe, not a multitasking example. [Arduino implementation][arduino-sd]
- M5GFX's CoreS3 initialization calls `_set_sd_spimode(..., GPIO_NUM_4)` before display traffic. Preserve that behavior when modifying startup. If replacing the board driver: initialize power/bus, idle other CS lines high, put the card into SPI mode, then communicate with other SPI devices. A card still in SD mode may respond to unrelated traffic despite its CS being high. [M5GFX][gfx], [IDF shared-bus startup][sharing]
- Native C/IDF: `SDSPI_HOST_DEFAULT`, `sdspi_device_config_t`, `esp_vfs_fat_sdspi_mount`; attach to the existing SPI2 bus when the board driver owns it. Preserve M5GFX bus locking; coordinate lifecycle/multi-call operations at application level too. Do not independently initialize/free the display's bus. [SDSPI guide][sdspi], [M5GFX bus backend](https://github.com/m5stack/M5GFX/blob/0.2.28/src/lgfx/v1/platforms/esp32/common.cpp)
- Use a known FAT32 card first. exFAT is not enabled by default in IDF; a large factory-formatted card can fail to mount despite working electrically. Set `format_if_mount_failed=false`; report mount failure distinctly from a missing card. [FatFs][fatfs]
- The M5 example requests 25 MHz; this is not a guaranteed throughput or a universal maximum. Reduce the transfer clock when debugging integrity, and inspect rail stability, CS timing and bus loading before increasing it. Protocol probing starts slowly. [M5 example][m5-sd], [signal loading][sharing]

## Concurrent logging design

- Give storage one worker and a bounded queue. Serialize display/SD bus access with a common application mutex; complete display DMA and end any held display transaction before SD access. Keep each bus hold short; release before waiting for network or sensor work. A task per core does not make shared wires concurrent.
- Start with batched sequential writes, e.g. 4–16 KiB in 512-byte multiples, then measure worst-case write/sync latency. Keep DMA staging internal/aligned; PSRAM can hold backlog if the driver copies safely. Size the queue from measured stalls and record dropped samples. FatFs sector-aligned multi-sector I/O reduces overhead. [FatFs performance notes][appnote]
- Use `FILE_APPEND` for Arduino append; `FILE_WRITE` is truncating write mode. Check returned byte counts and errors. For C use explicit append/create semantics and handle short writes. [Arduino FS definitions][fs]
- Set a durability interval in time/bytes: C `fflush(FILE*)` **then** `fsync(fileno(FILE*))`, Arduino `File.flush()`, or raw FatFs `f_sync`. Check the APIs that return status. Flushing reduces the loss window; it does not make FAT transactional or guarantee survival of the card controller's own power loss. [FatFs synchronization][sync], [Arduino flush implementation][vfs]
- Application format recommendation: sequence ID + monotonic acquisition time + UTC/time-valid flag + schema/units + length/CRC; rotate bounded files well before FAT32's **4 GiB − 1 byte** file limit. On restart recover complete records and report a partial tail. Avoid rewriting the entire dataset per sample. [FAT limits/power interruption][appnote]
- Eject/shutdown: stop producers → drain queue → sync/close → unmount (`SD.end()` / `esp_vfs_fat_sdcard_unmount`) → remove card/power. Unexpected removal becomes unavailable storage; never autoformat to recover. **Insertion/power cycling requires a quiesced display/bus and a fresh SPI-mode handshake before display traffic resumes**; startup-only `_set_sd_spimode` does not establish hotplug support. [Startup restrictions][sharing]
- ALDO4-off alone does not isolate an inserted card: shared SPI lines/pull-ups can back-power it. Quiesce or electrically isolate the relevant signals before power-gating, then measure the rail. [Schematic p5](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/490/Sch_M5_CoreS3_v1.0.pdf). For USB MSC export, give the host or firmware exclusive filesystem ownership.

## Lookup / verification triggers

| Trigger | Investigate |
| --- | --- |
| Mounts intermittently | FAT vs exFAT, ALDO4 rail, TF_SW, GPIO35 direction, CS3/4, SPI clock, add-on loads; test a cold power cycle, not only software reset. |
| Slow or unstable logger | `CONFIG_FATFS_IMMEDIATE_FSYNC`, per-file cache, `max_files`, allocation unit, `disk_status_check_enable`; [FatFs options][fatfs]. Measure latency percentiles while UI, BLE and Wi-Fi upload are active. |
| Large exports | Stream closed files in bounded chunks; checkpoint upload acknowledgements independently. If Parquet is needed, assess a host-side converter first; embedded writers must budget row groups, encoding and footer finalization. [Parquet format](https://parquet.apache.org/docs/file-format/). |
| Acceptance | Write/read/CRC check across rotations; full-card, removal/reinsert and power interruption during write/sync; verify previous files survive and queue overflow is observable. Record card model/capacity/filesystem and SDK versions. |

[m5-sd]: https://docs.m5stack.com/en/arduino/m5cores3/sdcard
[gfx]: https://github.com/m5stack/M5GFX/blob/0.2.28/src/M5GFX.cpp
[power]: https://github.com/m5stack/M5Unified/blob/0.2.21/src/utility/Power_Class.cpp
[board]: https://docs.m5stack.com/en/core/CoreS3
[arduino-sd]: https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/SD/src/SD.cpp
[sharing]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/sdspi_share.html
[sdspi]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/sdspi_host.html
[fatfs]: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/storage/fatfs.html
[appnote]: https://elm-chan.org/fsw/ff/doc/appnote.html
[sync]: https://elm-chan.org/fsw/ff/doc/sync.html
[fs]: https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/FS/src/FS.h
[vfs]: https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/FS/src/vfs_api.cpp
