#pragma once

#include "ble_sync.h"

#include <cstdint>

namespace telemetry {
struct PmsSnapshot {
  std::uint16_t values[12]{};
  std::uint32_t age_ms = 0;
  std::int64_t received_mono_us = 0;
  std::uint32_t frames = 0;
  std::uint32_t checksum_errors = 0;
  std::uint32_t length_errors = 0;
  std::uint8_t firmware = 0;
  std::uint8_t error = 0;
  bool present = false;
};

// Called once after M5.begin and SD mount. One worker owns all subsequent SD
// IO.
void begin_logger(bool sd_mounted);
// Call on every application loop; takes one snapshot when the 10s deadline is
// due.
void poll_logger(const PmsSnapshot &pms);
// Serialize display transactions with the storage worker on the shared SPI bus.
void lock_display();
void unlock_display();
// Load device settings and start the sync links (docs/ble-sync-protocol.md):
// BLE with the stored pairing mode, and the Wi-Fi/LAN task (radio stays off
// until configured). `display_detected` seeds the first-boot pairing default.
// Call after begin_logger(); false when BLE could not start.
bool start_links(bool display_detected);
// Called from the NimBLE host task or the LAN task: queue a control request
// for the storage worker (Wi-Fi scans are diverted to the LAN task). False
// when the command queue is full (the caller reports busy on its link).
bool enqueue_request(const ble::ControlRequest &request);
// Identities for mDNS TXT records; valid after begin_logger().
const char *station_text_id();
const char *device_text_id();
const char *firmware_text_id();
} // namespace telemetry
