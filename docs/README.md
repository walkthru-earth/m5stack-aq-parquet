# CoreS3 reference router

Target: **M5Stack CoreS3 / ESP32-S3**, C and/or C++. Accessories are optional, separately described hardware; no attached module is assumed. Research snapshot: **2026-09-06**; references are source-checked, not a hardware-tested firmware configuration.

Entry point: root [`AGENTS.md`](../AGENTS.md). It routes here and holds the host-environment and hard rules.

| When working on… | Read |
| --- | --- |
| Wiring, GPIO allocation, onboard peripherals, power, USB | [Core hardware](cores3-hardware.md) |
| C/C++, framework choice, library versions, memory, driver ownership | [Development](cores3-development.md) |
| Wi-Fi, Bluetooth LE, ESP-NOW, channels and coexistence | [Wireless](cores3-wireless.md) |
| Built-in microSD slot, shared SPI, logging, removal/recovery | [Storage](cores3-storage.md) |
| Any optional Unit, Module, Base, or third-party peripheral | [Add-on integration](addons.md), then its individual reference |
| Optional M134/PMSA003 air-quality accessory | [Air-quality add-on](addon-air-quality.md) |

Maintenance: keep board facts in hardware, core dependency recommendations in development, accessory facts/driver pins in `addon-<name>.md`. Preserve verified wiring, conflicts, protocol edge cases, and source links; use a keyword/link for routine APIs. Record SKU/revision and uncertainty when sources disagree. A release labeled “latest” is a dated observation, not a floating dependency pin. If an M5 CDN link fails, resolve the resource again through its linked product page.
