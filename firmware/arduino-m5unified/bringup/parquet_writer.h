#pragma once

#include <stddef.h>
#include <stdint.h>

namespace telemetry {

constexpr size_t kMaxColumns = 96;
constexpr size_t kMaxRows = 65536;

enum class PhysicalType : uint8_t { Int32 = 1, Int64 = 2, Float = 4 };

// values points at the first field in an array of rows, or a contiguous column.
// A null valid pointer makes the field required. Otherwise each strided byte is
// zero for null, nonzero for present. Buffers must remain immutable until
// return.
struct Column {
  const char *name = nullptr;
  PhysicalType type = PhysicalType::Int32;
  const void *values = nullptr;
  size_t stride = 0;
  const uint8_t *valid = nullptr;
  size_t valid_stride = 1;
};

struct KeyValue {
  const char *key;
  const char *value;
};

// Return true ONLY when every byte was accepted; false aborts without retries.
using Sink = bool (*)(void *, const uint8_t *, size_t);

// Caller-owned (static, heap, or PSRAM): no allocation or page-sized stack use.
struct Workspace {
  uint64_t offsets[kMaxColumns];
  uint64_t sizes[kMaxColumns];
  uint8_t buffer[512];
};

struct Result {
  bool ok;
  // Bytes accepted by successful sink calls. A failing sink may have accepted
  // part of its final block, which this interface cannot count.
  uint64_t bytes_written;
  const char *error;
};

// Flat numeric schema, one row group, one PLAIN DataPageV1 per column,
// UNCOMPRESSED, RLE definition levels, Thrift Compact footer. Empty input
// writes zero row groups. No logical timestamp annotation: use explicit unit
// names. This finalizes file contents only. Caller owns close/sync/rename and
// recovery.
Result write_parquet(Sink sink, void *context, const Column *columns,
                     size_t column_count, size_t row_count,
                     Workspace &workspace, const KeyValue *metadata = nullptr,
                     size_t metadata_count = 0);

} // namespace telemetry
