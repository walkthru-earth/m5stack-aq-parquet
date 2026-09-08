#pragma once

#include "parquet_writer.h"
#include "telemetry_dictionary_digest.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace telemetry {
namespace contract {
constexpr std::int32_t kSchemaVersion = 2;
constexpr const char *kSchemaName = "cores3-telemetry-v2";
constexpr const char *kFirmware = "arduino-cores3-parquet-v3";
constexpr const char *kDictionaryUri =
    "https://github.com/walkthru-earth/m5stack-aq-parquet/blob/main/"
    "firmware/arduino-m5unified/bringup/telemetry_fields.inc";
// Version the acquisition procedure separately from the physical file codec.
constexpr const char *kConfigurationId = "cores3-acquisition-v1";
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":18,\"pms_uart_tx\":17,"
    "\"pms_baud\":9600,\"m5unified\":\"0.2.21\",\"ltr553\":\"raw-v1\","
    "\"snapshot\":\"latest-available-not-average\"}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_ns estimates snapshot start; collection_completed_mono_us "
    "bounds sequential reads; pms_received_mono_us is last checksum-valid UART "
    "frame receipt, not sensor phenomenon time; result/ingestion time and "
    "clock uncertainty unknown; no retroactive UTC assignment";

enum Field : std::size_t {
#define FIELD(name, type, procedure, unit, validity) name,
#include "telemetry_fields.inc"
#undef FIELD
  field_count
};
struct Definition {
  const char *name;
  PhysicalType type;
  const char *procedure;
  const char *unit;
  const char *validity;
  const char *property_uri;
};
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity)                           \
  {#name, PhysicalType::type, #procedure,                                      \
   unit,  #validity,          "urn:walkthru-earth:cores3:property:" #name},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 77, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");

struct Sample {
  std::array<std::int64_t, field_count> data{};
  std::array<std::uint8_t, field_count> valid{};
  template <typename T> void set(Field field, T value) {
    static_assert(sizeof(T) <= sizeof(std::int64_t), "numeric field too large");
    std::memcpy(&data[field], &value, sizeof(value));
    valid[field] = 1;
  }
  void integer(Field field, std::int32_t value) { set(field, value); }
  void counter(Field field, std::int64_t value) { set(field, value); }
  void number(Field field, float value) {
    if (std::isfinite(value))
      set(field, value);
  }
};

inline void prepare_columns(Column *columns, Sample *rows) {
  for (std::size_t i = 0; i < field_count; ++i)
    columns[i] = Column{kFields[i].name, kFields[i].type,   &rows[0].data[i],
                        sizeof(Sample),  &rows[0].valid[i], sizeof(Sample)};
}

// Inputs are a single coherent anchor snapshot taken under the clock mutex.
// Caller provides a fresh zero-initialized row. Unknown UTC stays null.
inline void apply_clock(Sample &row, std::int64_t now, std::int64_t mono_anchor,
                        std::int64_t utc_anchor, std::int32_t generation) {
  row.integer(clock_status, generation ? 1 : 0);
  row.integer(clock_epoch, generation);
  if (generation) {
    row.counter(event_time_utc_ns, utc_anchor + (now - mono_anchor) * 1000);
    row.counter(clock_anchor_mono_us, mono_anchor);
    row.counter(clock_anchor_utc_ns, utc_anchor);
  }
}
} // namespace contract
} // namespace telemetry
