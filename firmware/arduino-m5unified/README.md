# Trial, Arduino with M5Unified

**Status, not started. No code written, no build, never flashed.**

This directory exists to record a partially completed toolchain bootstrap so the next session does not repeat the download. There is no firmware here yet.

## What this trial is meant to prove

Bring-up only. Produce one diagnostic that reports what the board actually is, so the source-checked claims in `docs/` can be confirmed or corrected. It is a throwaway instrument, not the eventual application.

Chosen over native ESP-IDF for the first bring-up because M5Unified already implements every CoreS3 peripheral the diagnostic needs to interrogate, the AXP2101 PMIC, the AW9523 expander, the microSD card on the shared SPI bus, IMU, RTC, touch and display. That makes the diagnostic mostly calls into working drivers rather than new driver code, and the SDK download is roughly half the size of a full ESP-IDF install.

## Required report sections, in priority order

1. Chip identity, revision, cores, flash size and mode, MAC, reset reason.
2. Memory, internal heap and **detected PSRAM size**. This one matters most, because PSRAM is the single documented claim that cannot be confirmed from eFuse. See `docs/bench-verified.md`.
3. Internal I2C scan on SDA GPIO12 and SCL GPIO11, each address annotated against the table in `docs/cores3-hardware.md`.
4. AXP2101 power, battery voltage and percentage, charging state, VBUS present, via the `M5.Power` API and never raw expander writes.
5. microSD mount and size. `SPI.begin(36, 35, 37, 4)` then `SD.begin(4, SPI, 25000000)`.
6. Bounded PMSA003 probe on GPIO18 RX and GPIO17 TX at 9600. A timeout means no module, never zero pollution.

The LCD summary page is nice to have. Drop it before dropping any section above.

## Toolchain state, as of 2026-09-06

Bootstrap was interrupted partway. Roughly 1.3 GB is already cached under `$M5_TOOLCHAIN_ROOT`, default `~/.cache/m5stack-aq-parquet/toolchains`.

| Path | Size | State |
| --- | --- | --- |
| `bin/` | 34 MB | arduino-cli, present |
| `arduino/` | 228 MB | esp32 core, **partial download** |
| `espressif/` | 497 MB | ESP-IDF tools pulled by the parked IDF trial, unrelated to this one |

Two strays were created before the bootstrap was redirected into the cache, and neither is used by this trial. `arduino-cli` 1.5.1 installed through Homebrew, and a partial ESP-IDF clone at `~/esp/esp-idf` taking 563 MB. Both are safe to delete.

`setup.sh` was never written. Writing it, pinned, is the first task on resuming.

## Next steps

1. Write `setup.sh`, pinned, downloading arduino-cli into `$M5_TOOLCHAIN_ROOT/bin` and installing the esp32 core plus M5Unified and M5GFX, with `ARDUINO_DIRECTORIES_*` pointed inside the cache so nothing pollutes the home directory.
2. Confirm the CoreS3 FQBN with `arduino-cli board listall | grep -i cores3`.
3. Set PSRAM to **Quad**, flash to **16 MB**, and a partition scheme with room for the app. Octal PSRAM produces a board that does not boot.
4. Write the sketch, build it, then hand the binary to the flashing step. Flashing is done from the root pixi tasks, never from inside a trial.

## Before flashing anything

The board ships with a UIFlow MicroPython image and a full 16 MB backup already exists. Flashing this trial overwrites that image. Restore goes through `pixi run restore`. Read the "Do not brick the board" section of the root `AGENTS.md` first.

**Last verified on hardware, never.**
