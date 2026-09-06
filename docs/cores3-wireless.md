# CoreS3 wireless: constraints and tuning

Load for Wi-Fi/BLE/ESP-NOW design or debugging. Verified 2026-09-06; target **ESP32-S3**.
SDK/library pins: [development](cores3-development.md). Linked `stable` guides currently describe IDF 6.1; select the installed SDK version before copying APIs/Kconfig.

## Radio and channel model

| Requirement | CoreS3 behavior / decision |
|---|---|
| Wi-Fi | 2.4 GHz 802.11 b/g/n (Wi-Fi 4), HT20/HT40; no integrated 5/6 GHz or Wi-Fi 6. [SoC][soc] |
| Bluetooth | Bluetooth 5 LE; no Classic BR/EDR, SPP `BluetoothSerial`, A2DP or HFP. A BLE GATT UART service is possible. [Chip capability][bt-architecture] |
| STA + SoftAP | One shared home channel; upstream STA channel wins and SoftAP migrates. Multiple AP clients are connections, not independent channels. [Home channel][wifi-overview] |
| BLE + Wi-Fi | One shared RF resource, time divided by coexistence; two CPU cores do not create two radios. [Coexistence][coex] |
| HT40 / BLE channels | HT40 bonds spectrum for one Wi-Fi link; BLE hopping/multiple links do not add independent Wi-Fi interfaces. [SoC][soc], [BLE stack][bt] |
| Independent channels/bands | Evaluate a second MCU/radio via UART/SPI or supported USB/network coprocessor; keywords: ESP-Hosted, ESP-AT, USB host class driver, VBUS budget, antenna isolation. |

## ESP-NOW with infrastructure Wi-Fi

- Start Wi-Fi before ESP-NOW. Normally every communicating peer must share the active channel; peer `channel=0` uses the current channel, **not** channel discovery. Select the enabled STA/AP interface correctly. [ESP-NOW][now]
- Architecture choice: managed fixed AP channel, explicit peer rediscovery after STA roaming/AP channel changes, or a second radio. Test channel changes after association, not only boot-time communication.
- Off-channel tools: `esp_now_switch_channel_tx()` and `esp_now_remain_on_channel()` exist in both [IDF 5.5.5][now55] and [6.1][now61]. They schedule temporary channel visits, not simultaneous reception. Check bundled headers, completion events and return codes; measure disruption under active Wi-Fi/BLE traffic.
- Match callback typedefs to installed headers: these snapshots use `const esp_now_send_info_t *` for send information; old MAC-pointer callback examples need migration. Copy received bytes/metadata before callback return. [Headers][now55]
- Send success acknowledges the MAC layer only: application sequence IDs, acknowledgement, retry/backoff and deduplication handle loss. Queue callback work; pace sends using completion rather than flooding the driver. [ESP-NOW][now]
- Mixed legacy nodes: keep payloads ≤250 bytes unless every receiver supports v2's 1470-byte payload. Explicit PMK/LMK and encrypted unicast protect peers; multicast encryption is unsupported. Read actual target limits/Kconfig instead of assuming a universal encrypted-peer count. [ESP-NOW][now]

## BLE coexistence and scheduling

- Start with ESP-IDF NimBLE for BLE-only memory efficiency; Arduino C++ wrapper/version lives in [development](cores3-development.md). Choose one host stack and inspect required features before enabling it. [Stacks][bt]
- Enable/check `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`. Espressif's matrix marks STA+BLE supported; SoftAP clients+BLE and sniffer+BLE have unstable performance; ESP-NOW RX+BLE is supported in STA mode. Verify your exact combination. [Matrix][coex]
- When profiling supports it: keep BT controller and host on one core, Wi-Fi task on the other. Keywords: `CONFIG_BT_CTRL_PINNED_TO_CORE_CHOICE`, `CONFIG_BT_NIMBLE_PINNED_TO_CORE_CHOICE`, `CONFIG_ESP_WIFI_TASK_CORE_ID`. This changes CPU contention, not RF airtime. Avoid blindly pinning application tasks onto those cores. [Coexistence options][coex]
- Tune BLE scan window/interval, active/passive scan, connection interval, PHY, MTU and data length together; test discovery latency and packet loss during Wi-Fi reconnects. Continuous scanning and maximum throughput compete for airtime. [Coexistence][coex], [NimBLE options][nimble-config]
- Multiple BLE connections require host **and** controller capacity: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`, S3 `CONFIG_BT_CTRL_BLE_MAX_ACT`; budget advertising/scanning activities, buffers and connection events too. [NimBLE Kconfig][nimble-config]

## Connection lifecycle and power

- Give one application task ownership of network state. Event handlers/ESP-NOW callbacks enqueue bounded work; filesystem writes, sensor waits, JSON and TLS belong in workers. Keep queue overflow counters. [Station events][station], [ESP-NOW][now]
- Gate IPv4 sockets/MQTT/HTTPS on `IP_EVENT_STA_GOT_IP`, not merely association. On disconnect/IP change, invalidate/recreate affected sockets. Reconnect using bounded backoff with jitter; distinguish intentional disconnect from failed authentication. [Lifecycle][station]
- Serialize scans with connection attempts. Reconnect loops can starve scans; limit retry bursts. Retrieve/free scan records once, and shorten/channel-limit scans only after measuring missed APs. [Scan lifecycle][station]
- `WIFI_PS_NONE` reduces receive latency and increases power; it does **not** disable coexistence time slicing. Compare against `WIFI_PS_MIN_MODEM` with your AP's DTIM, BLE load and battery budget. SoftAP does not buffer multicast for sleeping clients. [Power save][power]
- Set country/channel policy through supported Wi-Fi APIs for deployment; channel availability derives from that policy and AP information. Do not treat a scan's numeric channel range as universal permission. [Country configuration][wifi-overview]

## On-demand investigation

| Trigger | Keywords / evidence to gather |
|---|---|
| Throughput or stalls | [`wifi/iperf`, Wi-Fi buffer sizing, AMPDU, lwIP windows][power]; compare radio-only baseline with display/camera/SD workloads; RSSI, channel, latency percentiles, loss, internal heap minimum, queue drops. |
| Field reliability | Test AP reboot/channel move, wrong credentials, DHCP/DNS loss, TLS reconnect, simultaneous BLE scans and Wi-Fi upload, queue saturation, sleep/wake and long soak. Log disconnect reason and SDK/config identity. |
| WPA/TLS | [WPA3-SAE, PMF, WPA2-Enterprise][security]; [ESP-TLS certificate bundle, hostname verification, time validity][tls]. Keep credentials out of source/logs. |
| OTA | [HTTPS OTA, partial download/resumption, signed images][ota]; `ota_0/ota_1`, rollback, post-boot health confirmation; power-cut and invalid-certificate/image tests. |
| Provisioning / mesh / sensing | SDK keywords only: `wifi_provisioning`, `protocomm`, `wifi/roaming`, `esp_netif`, MQTT outbox, BLE Mesh, ESP-WIFI-MESH, CSI. Confirm S3 support and dependency versions before adopting. |

[soc]: https://www.espressif.com/en/products/socs/esp32-s3
[bt-architecture]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/bt-architecture/overview.html
[bt]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/index.html
[wifi-overview]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/overview.html
[coex]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html
[now]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html
[now55]: https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_wifi/include/esp_now.h
[now61]: https://github.com/espressif/esp-idf/blob/v6.1/components/esp_wifi/include/esp_now.h
[nimble-config]: https://github.com/espressif/esp-idf/blob/v6.1/components/bt/host/nimble/Kconfig.in
[station]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/station-scenarios.html
[power]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html
[security]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-security.html
[tls]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_tls.html
[ota]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/esp_https_ota.html
