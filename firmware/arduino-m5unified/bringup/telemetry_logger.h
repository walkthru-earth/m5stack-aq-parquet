#pragma once

#include <cstdint>

namespace telemetry {
struct PmsSnapshot {
  std::uint16_t values[12]{};
  std::uint32_t age_ms = 0;
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
} // namespace telemetry
