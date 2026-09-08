# Sensor telemetry, Parquet and cloud pipeline

[Router](README.md) · Read for the measurement record, near-time buffer, durable segments, upload protocol, Parquet conversion and cloud table design. Hardware-level SD rules remain in [CoreS3 storage](cores3-storage.md). Research snapshot: **2026-09-08**.

## Current decision

The CoreS3 should use a **store-and-forward pipeline**:

```text
sensor parsers -> bounded PSRAM queue -> one SD writer -> immutable raw segments
                                                        |
                                                        v
                                      idempotent HTTPS upload
                                                        |
                                                        v
                            object storage -> Parquet conversion -> Iceberg table
```

Parquet is the analytics format in object storage. The first durable device format is a small, versioned append journal that can recover a valid prefix after power loss. This keeps the microcontroller responsible for acquisition and durability while Arrow runs where its memory footprint, dependencies and file-repair tools fit.

No mature, production-proven Parquet writer currently targets ESP32-S3. A direct writer remains a useful later experiment, but it must pass host conformance and repeated power-cut tests before it can replace the journal.

The active Arduino trial stays active. [ESP-IDF 6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1) is newer than the ESP-IDF 5.5.5 base inside Arduino-ESP32 3.3.11, but that alone does not justify a second trial. Start an IDF trial only after this implementation produces a measured driver, latency, memory or component limitation and record that finding in the trial README.

## Measurement contract

Every sample has an identity independent of wall-clock quality. Never turn a timeout, bad checksum or stale value into a zero measurement.

| Field | Requirement |
| --- | --- |
| `schema_version` | Unsigned integer; old decoders reject unsupported major versions cleanly. |
| `device_id` | Stable opaque identifier, provisioned separately from a human-readable name. |
| `boot_id` | Random 128-bit value generated once per boot. |
| `sequence` | Monotonic 64-bit counter within one boot; `device_id + boot_id + sequence` is the row identity. |
| `monotonic_us` | Acquisition time from the monotonic clock; always present. |
| `event_time_utc_ns` | Nullable UTC estimate. It is absent until time is trusted. |
| `clock_status` | Unsynced, estimated or synchronized, plus a clock epoch so time steps are visible. |
| Measurements | Fixed-width typed fields with units in the schema, not encoded into display strings. |
| Validity | Per-sensor status and validity bits; preserve missing, warming, stale, checksum error and device error distinctly. |
| Provenance | Firmware build, board revision, sensor firmware, configuration version and calibration identifier. |

Time synchronization writes an anchor record containing monotonic and UTC time plus uncertainty. Already-buffered samples are not rewritten merely because UTC later becomes available; the cloud can apply the anchor deterministically.

The first PMS schema carries both atmospheric and CF=1 PM1.0/PM2.5/PM10 values, all six cumulative particle-count channels, the PMS sensor-error byte and framing/checksum counters.

## Recoverable segment format

Version 1 is intentionally small and manually serialized with explicit byte order. It does not write a native compiler struct or rely on padding.

Each record has:

1. a synchronization marker and format version;
2. header and payload lengths;
3. record type and validity flags;
4. identity, sequence and time fields;
5. fixed-schema payload bytes;
6. standard CRC32 over the versioned header and payload.

Wrap the ESP-IDF CRC API and test it against host-generated known vectors. Its seed, continuation and final inversion rules are easy to apply incorrectly.

The final segment trailer records first and last sequence, record count, byte count and SHA-256 of the committed content. CRC catches a damaged record during streaming recovery; SHA-256 gives the uploader and cloud endpoint a stable content identity.

Use this lifecycle:

1. create a uniquely named `.open` file;
2. append complete records through one storage worker;
3. batch writes in 4–16 KiB, keeping the DMA staging buffer in internal RAM;
4. at a durability boundary, check the write count, call `fflush`, then `fsync`;
5. rotate on time or size, initially 10 minutes or 512 KiB;
6. write and sync the trailer, close, validate the file, then rename it `.ready`;
7. upload only `.ready` files;
8. delete only after an acknowledgement matches both size and SHA-256.

FAT rename is not assumed to be crash-atomic. At boot, scan both suffixes, validate complete records, recover the longest valid prefix of an `.open` file, and quarantine anything ambiguous. Never autoformat after a mount or recovery failure. Keep upload acknowledgements in a separate small journal so a reset cannot confuse “sent” with “durably accepted.”

An uncompressed journal is the baseline. Sensor data rates are low enough that predictable recovery matters more than early compression. Measure SD bytes, upload bytes, CPU time and energy before adding a codec.

## Memory, tasks and backpressure

- Sensor code parses into typed records and never writes the filesystem directly.
- A bounded queue in the verified 8 MB Quad PSRAM absorbs SD and network stalls. Capacity comes from a measured stall budget; allocation failure is a reported state.
- One storage task owns FatFS and all SD transactions. It shares the SPI2 bus with the display through one application lock and never waits for network work while holding that lock.
- DMA descriptors and a reusable 4–16 KiB sector-aligned SD staging buffer stay in internal DMA-capable RAM. Preserve an internal-memory reserve rather than letting general allocation consume it.
- A separate uploader reads only closed segments. Sampling continues offline until the retention limit is reached.
- Queue-full and card-full policy is explicit. Count lost records and emit a gap record when storage resumes; never silently overwrite unsent data.

Record sample jitter, queue high-water mark, drops, internal and PSRAM minimum free space, SD write p50/p99/max latency, sync latency, segment recovery count and upload retry count.

## Upload and cloud layout

Use HTTPS for immutable segment transfer. A stable object key can include schema version, device ID, boot ID, sequence range and digest:

```text
raw/v1/device_id=<id>/boot_id=<id>/<first>-<last>-<sha256>.seg
```

Create-only semantics such as `If-None-Match: *` make retries idempotent. After a timeout, query or retry the same key; never invent a new identity for the same bytes. The endpoint verifies length, SHA-256, record CRCs, sequence range and schema version before acknowledging.

MQTT QoS 1 can carry live gauges, alarms and device health. It is not the durable measurement source because duplicates and reconnect gaps are normal. Deduplicate any live copy by the same row identity.

The ingestion service retains raw segments, converts validated records to Parquet with Apache Arrow/PyArrow, and writes immutable data files. Partition by event day and a device bucket after observing query patterns; avoid one tiny Parquet file per device segment. Compact small files and commit them through an Apache Iceberg table so schema evolution, late data and atomic snapshot publication happen in the cloud.

OpenTelemetry belongs at the gateway and ingestion services. The device emits compact counters, reset reason, firmware/config versions, RSSI, heap watermarks, queue depth, SD state and last successful upload; cloud services translate those into metrics, logs and traces.

## C and C++ library assessment

These are dated observations, not floating dependencies.

| Project | Version checked | Fit for CoreS3 |
| --- | --- | --- |
| [Apache Parquet format](https://github.com/apache/parquet-format/releases/tag/apache-parquet-format-2.13.0) | 2.13.0, Apache-2.0 | The wire specification. Metadata and page headers use Thrift Compact Protocol; it is not an embedded writer library. |
| [Apache Arrow C++](https://arrow.apache.org/blog/2026/08/10/25.0.1-release/) | 25.0.1, Apache-2.0 | Production Parquet implementation for host/cloud. Its C++20 build and Arrow/Thrift dependency graph are unsuitable for this microcontroller. |
| [nanoarrow](https://github.com/apache/arrow-nanoarrow) | 0.9.0, Apache-2.0 | C runtime compiles to a few hundred KiB and writes Arrow IPC through `FILE*`, but it does **not** write Parquet or compressed IPC. |
| [Carquet](https://github.com/seladb/carquet/releases/tag/v0.7.0) | 0.7.0, MIT | Roughly 200 KiB C11 Parquet reader/writer candidate with custom allocators. It is pre-1.0, has no ESP32-S3 qualification and recently fixed conformance and memory-safety defects. |
| [DuckDB](https://github.com/duckdb/duckdb/releases/tag/v1.5.5) | 1.5.5, MIT | Excellent host validator, but its database surface and roughly 125 MB per-thread memory guidance are beyond the device budget. |
| [zcbor](https://github.com/NordicSemiconductor/zcbor/releases/tag/0.9.1) | 0.9.1, Apache-2.0 | Low-footprint C CBOR with CDDL-generated codecs, static-friendly operation and fragmented buffers. |
| [Espressif LZ4](https://components.espressif.com/components/espressif/lz4/versions/1.10.0/readme) | 1.10.0, BSD-2-Clause/Apache-2.0 integration | Block state is about 1–16 KiB; Frame defaults need at least 64 KiB and can use PSRAM. Parquet requires raw blocks under `LZ4_RAW`, never LZ4 Frame output. |
| [Zstandard](https://github.com/facebook/zstd/releases/tag/v1.5.7) / [IDF wrapper](https://components.espressif.com/components/rderr/esp-idf-zstd/versions/1.5.7/readme) | 1.5.7, BSD-3-Clause option | The wrapper reports about 65–220 KiB heap plus a dedicated task with roughly 16 KiB stack. Current IDF packaging is lightly used and third-party. |
| [Espressif zlib](https://components.espressif.com/components/espressif/zlib/versions/1.3.2~1) | 1.3.2~1, zlib License | Mature official component. Parquet `GZIP` requires a gzip wrapper, not a raw deflate or zlib stream. |
| [Snappy](https://github.com/google/snappy/releases/tag/1.2.2) | 1.2.2, BSD-3-Clause | Fast and widely readable by Parquet tools, but there is no official Espressif component. |

The archived standalone `parquet-cpp` project is not a path forward; its work moved into Apache Arrow. Full Arrow, Velox and DuckDB remain host/cloud tools.

Secondary findings: ESP-IDF's ROM `miniz.h` is a restricted legacy subset and must not be treated as current [miniz 3.1.2](https://github.com/richgel999/miniz/releases). [heatshrink](https://github.com/atomicobject/heatshrink) is small but not a Parquet codec. Current [MessagePack C/C++](https://github.com/msgpack/msgpack-c/releases) is maintained, while its available ESP registry wrapper is immature and offers no clear advantage over a generated fixed schema. [xxHash 0.8.3](https://github.com/Cyan4973/xxHash/releases/tag/v0.8.3) is useful elsewhere but cannot replace the standard CRC32 required by Parquet page checksums.

## Direct Parquet experiment

Parquet allows single-pass creation because column chunks are written before the footer, but readers locate metadata from the final footer. A power cut before that footer leaves no standard file, and appending rows to a closed file requires replacing the old footer. That recovery property is the main reason it is not the first device store.

If the journal pipeline passes durability tests, a direct writer experiment is limited to:

- one immutable file and one row group per rotation;
- required primitive columns only;
- PLAIN encoding and Data Page V1;
- uncompressed pages first;
- optional standard Parquet page CRC32;
- `LZ4_RAW` only after uncompressed fixtures pass;
- small, fixed page and row-group buffers with allocator limits;
- a minimal writer-only Thrift Compact implementation or audited Carquet subset;
- `.partial` -> sync -> close -> footer validation -> final rename.

Every generated file must open in Arrow/PyArrow and DuckDB, preserve nulls and exact integer values, and match host-generated fixtures. Test empty, one-row, maximum-value, invalid-value, clock-unsynced and multiple-page cases. Run repeated power cuts during data, page header, footer, sync and rename. Record bytes per row, compression time, heap high-water marks, sample jitter, SD latency and recovery outcome.

Do not use Parquet's deprecated `LZ4` enum, LZ4 Frame payloads, dictionaries, nested schemas, append-to-finalized files or one file per sample in the first experiment.

## Operational cases to implement

[Open Air's firmware](https://github.com/Open-Air-Foundation/firmware-one-openair) was inspected only as a checklist of field cases; no source, schema, protocol or architecture was imported. Our implementation must cover:

- independent sensor schedules and warm-up state;
- checksum resynchronization, invalid and stale measurements;
- bounded queues, explicit drop policy and offline retention;
- watchdog and reset-reason reporting;
- SD removal, full media, short writes and partial-tail recovery;
- Wi-Fi reconnect without blocking acquisition;
- idempotent upload and duplicate handling;
- configuration/calibration versions and safe rollback;
- signed OTA with health confirmation before marking a new image valid;
- visible counters for heap, RSSI, sensor failures, storage failures and upload backlog.

## Implementation order

1. **Complete:** live 10-second PMS display and serial measurement report.
2. Specify the byte-level journal and build host encode/decode fixtures.
3. Add the bounded PSRAM queue and single SD writer.
4. Verify recovery with forced resets and controlled power cuts.
5. Add immutable HTTPS upload and acknowledgement retention.
6. Convert raw segments to Parquet on the host and validate schema evolution.
7. Measure whether device-side compression or direct Parquet provides enough benefit to justify its extra failure surface.

Useful primary references: [Parquet file layout and recovery](https://github.com/apache/parquet-format/blob/master/README.md), [Parquet compression rules](https://github.com/apache/parquet-format/blob/master/Compression.md), [ESP-IDF FatFS behavior](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html), [ESP-IDF filesystem resilience guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/file-system-considerations.html), [ESP32-S3 heap capabilities](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/mem_alloc.html), and [Apache Iceberg specification](https://iceberg.apache.org/spec/).
