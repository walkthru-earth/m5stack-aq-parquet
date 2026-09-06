# AGENTS.md

Firmware project for **M5Stack CoreS3 (ESP32-S3)** in C and/or C++.
This repo holds **multiple parallel framework trials** against the same board and the same shared reference docs. Sensor modules are optional add-ons, never assumed to be attached.

## Repo layout

```
firmware/
  <framework>-<variant>/   one self-contained trial, own build system and lockfile
  common/                  optional shared code, consumed as a component
docs/                      shared, framework-neutral hardware and design references
tools/                     host-side scripts (log parsing, Parquet conversion)
backup/                    flash images taken before writes, git-ignored
```

Trial directory names state the framework first, for example `firmware/idf-cpp/`, `firmware/arduino-m5unified/`, `firmware/esp-rs/`.

### Rules for trials

1. A trial is **self-contained**. Its build files, `sdkconfig.defaults`, partition CSV, dependency lock, and toolchain identity live inside its own directory. Never reach into a sibling trial.
2. Shared knowledge lives in `docs/`, not duplicated per trial. If a trial discovers a board fact, promote it into the relevant `docs/` file rather than leaving it in one trial's README.
3. Every trial has a `README.md` recording the framework and pinned version, what the trial is meant to prove, current status (active, parked, or abandoned and why), the build and flash commands, and the date it was last verified on real hardware.
4. Trials may disagree. Do not refactor one trial to match another unless asked. An abandoned trial stays in the tree with its status recorded, because the reason it failed is the useful part.
5. Cross-trial comparison belongs in a top-level note, not inside a trial.

## Read on demand, not up front

`docs/README.md` is the router. Open only the topic files a task actually needs.

| Task touches | Open |
| --- | --- |
| Wiring, GPIO ownership, onboard peripherals, power, sleep, boot | `docs/cores3-hardware.md` |
| Toolchain, framework choice, library versions, memory, driver ownership | `docs/cores3-development.md` |
| Wi-Fi, BLE, ESP-NOW, channels, coexistence | `docs/cores3-wireless.md` |
| microSD, shared SPI bus, logging, power-loss recovery | `docs/cores3-storage.md` |
| Attaching any Unit, Module, Base, or third-party peripheral | `docs/addons.md`, then the matching `docs/addon-*.md` |
| PM2.5 air-quality module (M134 / PMSA003) | `docs/addon-air-quality.md` |
| Whether a board claim is measured or only source-checked | `docs/bench-verified.md` |

Each file links its upstream source. Follow the link when implementation detail is needed rather than guessing an API.

## Host environment

Host tooling is managed by **pixi** (`pixi.toml`, `pixi.lock`). Run project commands as `pixi run <task>` or `pixi run <tool>`. Add host packages with `pixi add`, never by pip-installing into the managed environment.

Firmware SDKs are not conda packages, so the project bootstraps them itself rather than asking you to install anything by hand. Each trial ships a pinned `setup.sh` and is driven by a root pixi task, so a clean machine needs only `pixi install` followed by that trial's setup task.

SDKs are downloaded into `$M5_TOOLCHAIN_ROOT`, which defaults to `~/.cache/m5stack-aq-parquet/toolchains`. That path sits outside the repo on purpose, so several git worktrees share one copy and nothing large is ever written inside the tree. Build inside the SDK's exported environment so pixi does not shadow SDK-selected tools. See `docs/cores3-development.md`.

## Do not brick the board

Run these in order the first time a physical CoreS3 is connected, before any write.

| Step | Command | Why |
| --- | --- | --- |
| 1 | `pixi run ports` | Confirm which serial device is the board. Flashing the wrong port is the most common way to damage an unrelated device. |
| 2 | `pixi run chip` | Read-only identification. Refuses if the target is not an ESP32-S3. |
| 3 | `pixi run flash-id` | Confirm 16 MB flash before assuming a partition layout. |
| 4 | `pixi run efuse` | Read-only. Confirms flash type and that no secure-boot or flash-encryption fuse has been burned, which is what makes the board recoverable. |
| 5 | `pixi run backup` | Full 16 MB image into `backup/`, timestamped. Do this once per board before the first write, and keep it. |
| 6 | `ls -l backup/` | **Confirm the file exists and is 16777216 bytes.** A failed read can still exit zero behind a shell pipeline. Do not skip this. |

`pixi run restore backup/<file>.bin` writes a saved image back. It is destructive, so name the file explicitly. `pixi run backup-partitions backup/<file>.bin` shows what was on the board, without touching hardware.

Further rules.

- CoreS3 is **Quad PSRAM, 16 MB flash**. Selecting Octal PSRAM because another S3 board uses it produces a board that does not boot.
- Never add pulls or drivers to strapping pins 0, 3, 45, 46. They already carry board functions.
- Do not write eFuses. There is no undo. Nothing in this project needs them. Reading them with `pixi run efuse` is safe.
- **Never pass a high `--baud` to esptool on this board.** Measured, the default read 16 MB in 97 seconds, while `--baud 921600` corrupted the stream and aborted. The transport is native USB-Serial/JTAG, so a higher requested baud gains nothing.
- Recovery from a bad image is hold RST about 3 seconds until the green LED, then release for download mode. Verify recovery works before flashing anything that reconfigures power or USB mode.
- `docs/cores3-hardware.md` covers rail assignments and expander bits. Raw expander writes can reset the screen or reverse the supply path, so use the `M5.Power` API.

## Code quality gates

`pixi run fmt`, `pixi run fmt-check`, `pixi run lint` (cppcheck over `firmware/`), `pixi run hooks` to install pre-commit.

## Hard rules

1. One owner per peripheral or controller. Do not initialize the same I2C, SPI, or I2S controller from two driver families.
2. Treat every version number in the docs as a dated observation. Recheck upstream before changing a dependency, and pin exact versions plus lockfiles.
3. Do not copy Core or Core2 GPIO numbers. CoreS3 pin assignments differ.
4. Record the board revision and SKU when a source is ambiguous, and say so in the doc rather than picking a guess silently.
5. Most documentation here is source-checked, not bench-tested. Anything confirmed on real hardware belongs in `docs/bench-verified.md` with the date and the method, and that file wins over the others where they disagree. Do not promote a claim into it without an actual measurement.

## Keeping docs current

Board facts stay in `cores3-hardware.md`, dependency guidance in `cores3-development.md`, accessory facts in `docs/addon-<name>.md`. Preserve verified wiring, conflicts, protocol edge cases, and source links. For routine APIs an agent can look up on its own, leave a keyword and a link instead of prose.
