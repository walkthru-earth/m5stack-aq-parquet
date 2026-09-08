// Build through pixi; uses exactly the firmware writer on the host.
// Usage: parquet_fixture output.parquet [rows=90] [columns=8]
#include "../firmware/arduino-m5unified/bringup/lz4_codec.h"
#include "../firmware/arduino-m5unified/bringup/parquet_writer.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
struct Row {
  int64_t time_ms;
  int32_t counter;
  float temperature;
  int64_t signed_extreme;
  uint8_t alternating;
  uint8_t absent;
  uint8_t present;
};

bool file_sink(void *context, const uint8_t *bytes, size_t length) {
  return fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

struct FailingSink {
  size_t calls = 0;
};

bool fail_sink(void *context, const uint8_t *, size_t) {
  auto &state = *static_cast<FailingSink *>(context);
  return ++state.calls == 1;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 5)
    return 2;
  const size_t count = argc > 2 ? strtoul(argv[2], nullptr, 10) : 90;
  const size_t column_count = argc > 3 ? strtoul(argv[3], nullptr, 10) : 8;
  if (count > telemetry::kMaxRows || column_count < 8 ||
      column_count > telemetry::kMaxColumns)
    return 2;
  std::vector<Row> rows(count ? count : 1);
  for (size_t i = 0; i < count; ++i) {
    rows[i] = {1700000000000LL + static_cast<int64_t>(i) * 10000,
               i % 2 ? std::numeric_limits<int32_t>::max()
                     : std::numeric_limits<int32_t>::min(),
               static_cast<float>(i) * 0.25F - 10.0F,
               i % 2 ? std::numeric_limits<int64_t>::max()
                     : std::numeric_limits<int64_t>::min(),
               static_cast<uint8_t>(i % 2),
               0,
               7};
  }
  if (count > 4) {
    rows[0].temperature = -0.0F;
    rows[1].temperature = std::numeric_limits<float>::infinity();
    rows[2].temperature = -std::numeric_limits<float>::infinity();
    rows[3].temperature = std::numeric_limits<float>::quiet_NaN();
  }
  const Row &first = rows.front();
  using telemetry::PhysicalType;
  std::array<telemetry::Column, telemetry::kMaxColumns> columns;
  columns[0] = {"time_ms", PhysicalType::Int64, &first.time_ms, sizeof(Row)};
  columns[1] = {"counter", PhysicalType::Int32, &first.counter, sizeof(Row)};
  columns[2] = {"temperature", PhysicalType::Float, &first.temperature,
                sizeof(Row)};
  columns[3] = {"optional_counter", PhysicalType::Int32, &first.counter,
                sizeof(Row),        &first.alternating,  sizeof(Row)};
  columns[4] = {"all_null",  PhysicalType::Float, &first.temperature,
                sizeof(Row), &first.absent,       sizeof(Row)};
  columns[5] = {"signed_extreme", PhysicalType::Int64, &first.signed_extreme,
                sizeof(Row)};
  columns[6] = {"optional_extreme", PhysicalType::Int64, &first.signed_extreme,
                sizeof(Row),        &first.alternating,  sizeof(Row)};
  columns[7] = {"all_present", PhysicalType::Float, &first.temperature,
                sizeof(Row),   &first.present,      sizeof(Row)};
  std::array<std::array<char, 32>, telemetry::kMaxColumns> names{};
  for (size_t i = 8; i < column_count; ++i) {
    snprintf(names[i].data(), names[i].size(), "extra_%02zu", i);
    columns[i] = {names[i].data(), PhysicalType::Float, &first.temperature,
                  sizeof(Row)};
  }
  telemetry::Workspace workspace{};
  telemetry::Lz4Workspace lz4{};
  // Fixed-capacity adapter checks include incompressible/empty/short blocks.
  // These are host-only scratch arrays, not firmware task stack allocations.
  std::array<uint8_t, 1024> decoded{};
  auto bounded = lz4.configuration();
  uint32_t random = 0x6b40;
  for (size_t length = 0; length <= 1024; ++length) {
    for (size_t i = 0; i < length; ++i) {
      random = random * 1664525U + 1013904223U;
      bounded.raw[i] = static_cast<uint8_t>(random >> 24);
    }
    const size_t packed =
        bounded.compress(bounded.context, bounded.raw, length, bounded.encoded,
                         bounded.encoded_capacity);
    if (!packed || packed > bounded.encoded_capacity ||
        LZ4_decompress_safe(
            reinterpret_cast<const char *>(bounded.encoded),
            reinterpret_cast<char *>(decoded.data()), static_cast<int>(packed),
            static_cast<int>(length)) != static_cast<int>(length) ||
        std::memcmp(decoded.data(), bounded.raw, length) != 0)
      return 9;
  }
  auto compression = lz4.configuration();
  const bool compressed = argc == 5;
  // Large fixtures exercise format limits on the host only. Firmware remains
  // fixed at 1024-byte page scratch and 90 rows.
  std::vector<uint8_t> raw(count * 10 + 4);
  std::vector<uint8_t> encoded(LZ4_COMPRESSBOUND(raw.size()));
  compression.raw = raw.data();
  compression.raw_capacity = raw.size();
  compression.encoded = encoded.data();
  compression.encoded_capacity = encoded.size();
  const telemetry::KeyValue metadata[] = {
      {"sample_interval_ms", "10000"},
      {"fixture", "firmware writer interoperability"}};
  FILE *file = fopen(argv[1], "wb");
  if (!file)
    return 3;
  const auto result = telemetry::write_parquet(
      file_sink, file, columns.data(), column_count, count, workspace, metadata,
      2, compressed ? &compression : nullptr);
  const bool closed = fclose(file) == 0;
  if (!result.ok || !closed) {
    fprintf(stderr, "write failed: %s\n",
            result.error ? result.error : "close");
    return 4;
  }
  FailingSink failure;
  const auto failed = telemetry::write_parquet(
      fail_sink, &failure, columns.data(), column_count, count, workspace,
      metadata, 2, compressed ? &compression : nullptr);
  if (result.bytes_written > 512 && (failed.ok || failure.calls != 2))
    return 5;
  const auto invalid =
      telemetry::write_parquet(fail_sink, &failure, columns.data(),
                               telemetry::kMaxColumns + 1, count, workspace);
  if (invalid.ok || invalid.bytes_written)
    return 6;
  if (count) {
    auto broken = compression;
    broken.compress = [](void *, const uint8_t *, size_t, uint8_t *,
                         size_t) -> size_t { return 0; };
    failure.calls = 0;
    const auto codec_failed = telemetry::write_parquet(
        fail_sink, &failure, columns.data(), column_count, count, workspace,
        metadata, 2, &broken);
    if (codec_failed.ok || codec_failed.bytes_written != 4 ||
        failure.calls != 1)
      return 8;
    compression.raw_capacity = 0;
    const auto rejected = telemetry::write_parquet(
        fail_sink, &failure, columns.data(), column_count, count, workspace,
        metadata, 2, &compression);
    if (rejected.ok || rejected.bytes_written)
      return 7;
  }
  printf("rows=%zu columns=%zu bytes=%llu workspace=%zu\n", count, column_count,
         static_cast<unsigned long long>(result.bytes_written),
         sizeof(workspace));
  return 0;
}
