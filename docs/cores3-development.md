# CoreS3 C/C++ development

[Router](README.md) · Read for toolchain, dependency, driver, or runtime changes. Snapshot **2026-09-08**. The pinned Arduino-ESP32 3.3.11, M5Unified 0.2.21 and M5GFX 0.2.28 combination is built and bench-verified; other combinations remain source-checked only.

## Release snapshot

| Layer | Latest stable observed | Use / compatibility evidence |
| --- | --- | --- |
| ESP-IDF | [v6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1) | Native C/C++; explicit target `esp32s3`. [Refresh](https://github.com/espressif/esp-idf/releases/latest). |
| Arduino-ESP32 | [3.3.11](https://github.com/espressif/arduino-esp32/releases/tag/3.3.11) | Packaged core built on IDF **5.5.5**; [component manifest](https://github.com/espressif/arduino-esp32/blob/3.3.11/idf_component.yml) permits `>=5.3,<6.2`. Component builds need their own validation. [Refresh](https://github.com/espressif/arduino-esp32/releases/latest). |
| M5Unified | [0.2.21](https://github.com/m5stack/M5Unified/releases/tag/0.2.21) | Board services, power, inputs, audio, sensors; [requires M5GFX >=0.2.28](https://github.com/m5stack/M5Unified/blob/0.2.21/idf_component.yml). [Refresh](https://github.com/m5stack/M5Unified/releases/latest). |
| M5GFX | [0.2.28](https://github.com/m5stack/M5GFX/releases/tag/0.2.28) | CoreS3 panel/bus handling; fixes S3 SPI clock calculation and I²C clock stretching. [Refresh](https://github.com/m5stack/M5GFX/releases/latest). |
| NimBLE-Arduino, optional | [2.5.1](https://github.com/h2zero/NimBLE-Arduino/releases/tag/2.5.1) | Arduino BLE host; native IDF can use its bundled NimBLE. [Refresh](https://github.com/h2zero/NimBLE-Arduino/releases/latest). |
| esp32-camera, optional | [2.1.7](https://components.espressif.com/components/espressif/esp32-camera/versions/2.1.7/readme) | GC0308 capture; use CoreS3's actual pin/pixel-format configuration. [Refresh](https://components.espressif.com/components/espressif/esp32-camera). |
| LVGL, optional | [9.5.0](https://github.com/lvgl/lvgl/releases/tag/v9.5.0) | GUI above the panel driver; avoid v8 integration snippets. [Refresh](https://github.com/lvgl/lvgl/releases/latest). |

## Select and pin a coherent stack

- **Native IDF** for direct C APIs, Kconfig and resource control; add M5Unified/M5GFX as C++ components for board services. Their [CMake](https://github.com/m5stack/M5Unified/blob/0.2.21/CMakeLists.txt) supports component use; C modules can call a narrow `extern "C"` board wrapper. Pure C requires implementing the PMIC/expander/display setup in [hardware](cores3-hardware.md).
- **Arduino** for M5 examples and Arduino libraries; pin the board package and libraries. An Arduino-as-IDF-component build uses the selected IDF, whereas the packaged Arduino core has a bundled IDF; these are different dependency configurations. Keep the board definition, PSRAM/flash settings, USB mode and partition table explicit.
- Commit exact direct dependencies, `dependencies.lock` for IDF, `sdkconfig.defaults`, partition CSV, and toolchain identity. Re-resolve intentionally; never use `master`, `latest`, or an unbounded range as a reproducibility strategy. [Component Manager](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/dependencies_lock.html).
- PlatformIO lookup only if chosen: inspect the resolved framework version and board JSON; the platform package version is not the Arduino/IDF version. [Official platform](https://github.com/platformio/platform-espressif32), [pioarduino alternative](https://github.com/pioarduino/platform-espressif32). Do not transplant old `esp32-dev`/Core2 build flags.

## Local host tools versus firmware dependencies

- [Pixi manifest](../pixi.toml)/[lock](../pixi.lock) own the macOS ARM64 host tools. `clang-tools` includes clang-format, clangd and clang-tidy; pip supports SDK environment bootstrap. Use `pixi run <tool>`; declare Python dependencies through Pixi instead of manually pip-installing into its managed environment.
- **This project does not install SDKs by hand.** Each trial ships a pinned `setup.sh` driven by a root pixi task, and downloads into `$M5_TOOLCHAIN_ROOT` (default `~/.cache/m5stack-aq-parquet/toolchains`), outside the repo so worktrees share it. conda-forge carries no `esp-idf`, `arduino-cli` or `espflash` package, which is why bootstrap is a task rather than a dependency. It does carry `platformio` if a trial ever wants that route.
- **Budget the SDK download before promising a build.** Measured on 2026-09-06, roughly 1.3 GB landed in the cache in about 20 minutes and neither an Arduino nor an ESP-IDF bootstrap had finished. The resumed Arduino installation completed on 2026-09-08 and occupies 7.5 GB because the board package installs libraries and tools for all supported ESP32 targets; Arduino CLI adds 34 MB. Treat a first bootstrap as tens of minutes and several gigabytes, run exactly one at a time, and never relocate the cache midway, which restarts transfers already in flight.
- If installing an IDF manually instead, use the [Espressif Installation Manager](https://docs.espressif.com/projects/idf-im-ui/en/latest/); let its [versioned tool manifest](https://github.com/espressif/esp-idf/blob/v6.1/tools/tools.json) select Xtensa compiler, debugger, target-aware `esp-clangd` and SDK Python dependencies. Generic host Clang is not the ESP32-S3 compiler; firmware libraries belong in IDF/Arduino dependency management.
- Current [installer prerequisites](https://docs.espressif.com/projects/idf-im-ui/en/latest/prerequisites.html) support Python 3.14. Run SDK builds in the exported IDF environment so Pixi does not shadow SDK-selected tools. EIM/Arduino CLI and macOS DFU/QEMU prerequisites are separate setup when that workflow is selected; adding host packages alone does not install a firmware SDK.

## Runtime decisions worth retaining

- Set the C++ standard explicitly for application components; IDF 6.1 currently defaults to `gnu++26`. Choose features supported by every selected toolchain (e.g. C++23); do not infer Arduino's dialect from standalone IDF. Exceptions/RTTI default off; use RAII plus explicit error results. C++ `app_main` needs C linkage; zero-initialize SDK structs and assign fields to avoid C/C++ designated-initializer differences. [C++ constraints](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/cplusplus.html).
- Initialize board services once; disable unused M5 services in `M5.config()` before `M5.begin(cfg)`. One owner per peripheral/controller: do not separately initialize M5Unified I²C, Arduino `Wire`, and IDF I²C on the same controller. A mutex cannot reconcile independent driver state. [M5 configuration/source](https://github.com/m5stack/M5Unified/blob/0.2.21/src/M5Unified.hpp), [bus/device API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/i2c.html).
- New native drivers: `driver/i2c_master.h`, `driver/i2s_std.h`, `esp_adc/adc_oneshot.h`, `esp_adc/adc_continuous.h`, `driver/rmt_tx.h`, `driver/gptimer.h`; declare `esp_driver_*` dependencies (ADC: `esp_adc`). IDF 6 removes several legacy drivers; legacy I²C is EOL, with removal scheduled for 7.0. Do not mix old/new driver families. [6.0 migration](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/peripherals.html).
- Keep DMA descriptors and latency-critical buffers in internal RAM; use PSRAM for large frame/sample buffers only when the consuming driver supports it. `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`, `MALLOC_CAP_SPIRAM`, cache synchronization/alignment; PSRAM can become inaccessible during flash operations, subject to the configured XiP mode. [S3 RAM restrictions](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/external-ram.html).
- Use bounded queues between capture, UI, storage and networking; define overflow/drop policy. Keep callbacks short and serialize M5 updates/UI calls. Measure before pinning work to cores; ESP-IDF FreeRTOS stack sizes are **bytes**. [SMP specifics](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/freertos_idf.html).
- Upgrade traps: M5Unified 0.2.21 changes IO-expander pull/direction method signatures; IDF 6 uses managed `espressif/mqtt` ([migration](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/migration-guides/release-6.x/6.0/protocols.html#esp-mqtt)). Follow migration notes instead of suppressing compile errors.

## Lookup only when needed

| Need | Keywords / primary entry |
| --- | --- |
| Diagnose concurrency/memory | `heap_caps_get_largest_free_block`, stack high-water mark, task watchdog, heap poisoning, core dumps; [IDF diagnostics](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/fatal-errors.html) |
| Recover/ship firmware | USB Serial/JTAG, ROM download, `esp_https_ota`, A/B partitions, rollback self-test, NVS encryption; [OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/ota.html) |
| Camera/audio/UI | GC0308 RGB/YUV support, `fb_count`, PSRAM DMA, I²S ownership; [camera driver](https://github.com/espressif/esp32-camera), [M5 examples](https://github.com/m5stack/M5Unified/tree/0.2.21/examples), [LVGL threading](https://docs.lvgl.io/9.5/integration/overview.html#operating-systems-and-threads) |

Before accepting a firmware dependency upgrade: clean build + target-board smoke test for used peripherals; for concurrency changes, stress display/SD/radios together and record queue drops, watchdog resets and minimum internal heap. No firmware validation is implied by this documentation snapshot.
