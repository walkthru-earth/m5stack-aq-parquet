# m5stack-aq-parquet

Air-quality logging firmware for **M5Stack CoreS3 (ESP32-S3)**, writing to Parquet.

The repo holds **multiple parallel framework trials** against the same board, sharing one set of hardware reference docs.

- `AGENTS.md` is the entry point for humans and coding agents.
- `docs/` holds framework-neutral CoreS3 references (GPIO and power, development stack, wireless, microSD, optional add-ons).
- `firmware/<framework>-<variant>/` holds each self-contained trial.

## Getting started

```sh
pixi install
pixi run ports     # find the board
pixi run chip      # confirm it is an ESP32-S3, read-only
pixi run backup    # full flash image before the first write
```

Read the "Do not brick the board" section of `AGENTS.md` before flashing anything.

The ESP32-S3 cross-compiler is not managed by pixi. Install ESP-IDF separately, see `docs/cores3-development.md`.

## Hardware

M5Stack CoreS3 (K128), 16 MB flash, 8 MB Quad PSRAM. The PM2.5 air-quality module (M134 / PMSA003) is an optional add-on and is not assumed to be attached.

## License and attribution

Licensed under [CC BY 4.0](LICENSE), matching other Walkthru.Earth repositories.

**If you use any of this work, you must credit us visibly.** Attribution belongs
somewhere a reader actually sees it, such as your README, your documentation, your
about screen, or your paper. A buried comment in source does not count.

Minimum credit.

> Based on [walkthru-earth/m5stack-aq-parquet](https://github.com/walkthru-earth/m5stack-aq-parquet)
> by Walkthru.Earth, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

BibTeX.

```bibtex
@software{walkthru_m5stack_aq_parquet,
  author  = {Harby, Youssef},
  title   = {m5stack-aq-parquet: Air-quality logging firmware for M5Stack CoreS3},
  year    = {2026},
  url     = {https://github.com/walkthru-earth/m5stack-aq-parquet},
  license = {CC-BY-4.0},
  note    = {Walkthru.Earth}
}
```

State what you changed if you modified the work. Attribution does not imply we endorse your project.
