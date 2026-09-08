#include "parquet_writer.h"

#include <limits>
#include <string.h>

// Wire format references (checked 2026-09-08):
// https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift
// https://parquet.apache.org/docs/file-format/data-pages/encodings/
// https://github.com/apache/thrift/blob/master/doc/specs/thrift-compact-protocol.md
// This is a deliberately small format subset, not a general Parquet library.
namespace telemetry {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Parquet FLOAT requires IEEE 754 binary32");

constexpr uint8_t kI32 = 5, kI64 = 6, kBinary = 8, kList = 9, kStruct = 12;

class Output {
public:
  Output(Sink sink, void *context, Workspace &workspace)
      : sink_(sink), context_(context), workspace_(workspace) {}

  void bytes(const void *source, size_t size) {
    const auto *p = static_cast<const uint8_t *>(source);
    while (size && ok_) {
      size_t take = sizeof(workspace_.buffer) - used_;
      if (take > size)
        take = size;
      memcpy(workspace_.buffer + used_, p, take);
      used_ += take;
      position_ += take;
      p += take;
      size -= take;
      if (used_ == sizeof(workspace_.buffer))
        flush();
    }
  }
  void byte(uint8_t value) { bytes(&value, 1); }
  void varint(uint64_t value) {
    do {
      const uint8_t next = static_cast<uint8_t>(value & 127);
      value >>= 7;
      byte(next | (value ? 128 : 0));
    } while (value && ok_);
  }
  // All Thrift integer values generated here are nonnegative metadata.
  void integer(uint64_t value) { varint(value << 1); }
  void little_endian(uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      byte(static_cast<uint8_t>(value));
      value >>= 8;
    }
  }
  void string(const char *value) {
    const size_t length = strlen(value);
    varint(length);
    bytes(value, length);
  }
  void list(uint8_t type, size_t count) {
    if (count < 15) {
      byte(static_cast<uint8_t>((count << 4) | type));
    } else {
      byte(0xf0 | type);
      varint(count);
    }
  }
  void flush() {
    if (!ok_ || !used_)
      return;
    ok_ = sink_(context_, workspace_.buffer, used_);
    if (ok_)
      accepted_ += used_;
    used_ = 0;
  }
  bool ok() const { return ok_; }
  uint64_t position() const { return position_; }
  uint64_t accepted() const { return accepted_; }

private:
  Sink sink_;
  void *context_;
  Workspace &workspace_;
  size_t used_ = 0;
  uint64_t position_ = 0;
  uint64_t accepted_ = 0;
  bool ok_ = true;
};

// One object per nested struct keeps field-id deltas local, without a stack of
// dynamic protocol objects. Call end() explicitly to write the STOP marker.
class Struct {
public:
  explicit Struct(Output &output) : out_(output) {}
  void field(uint8_t id, uint8_t type) {
    const uint8_t delta = id - previous_;
    if (id > previous_ && delta <= 15) {
      out_.byte(static_cast<uint8_t>((delta << 4) | type));
    } else {
      out_.byte(type);
      out_.integer(id);
    }
    previous_ = id;
  }
  void number(uint8_t id, uint8_t type, uint64_t value) {
    field(id, type);
    out_.integer(value);
  }
  void string(uint8_t id, const char *value) {
    field(id, kBinary);
    out_.string(value);
  }
  void list(uint8_t id, uint8_t type, size_t count) {
    field(id, kList);
    out_.list(type, count);
  }
  void end() { out_.byte(0); }

private:
  Output &out_;
  uint8_t previous_ = 0;
};

size_t width(PhysicalType type) { return type == PhysicalType::Int64 ? 8 : 4; }

bool present(const Column &column, size_t row) {
  return !column.valid || column.valid[row * column.valid_stride] != 0;
}

size_t varint_size(uint64_t value) {
  size_t result = 1;
  while (value >>= 7)
    ++result;
  return result;
}

size_t present_count(const Column &column, size_t rows) {
  if (!column.valid)
    return rows;
  size_t count = 0;
  for (size_t row = 0; row < rows; ++row)
    count += present(column, row);
  return count;
}

// The bit width is 1 for a flat optional field. RLE runs need one value byte;
// their length is ULEB128(run_length << 1). DataPageV1 prefixes the stream
// length.
size_t definition_levels(const Column &column, size_t rows,
                         Output *output = nullptr) {
  size_t length = 0;
  size_t first = 0;
  while (first < rows) {
    const bool value = present(column, first);
    size_t end = first + 1;
    while (end < rows && present(column, end) == value)
      ++end;
    const uint64_t header = static_cast<uint64_t>(end - first) << 1;
    length += varint_size(header) + 1;
    if (output) {
      output->varint(header);
      output->byte(value ? 1 : 0);
    }
    first = end;
  }
  return length;
}

size_t payload_size(const Column &column, size_t rows) {
  return present_count(column, rows) * width(column.type) +
         (column.valid ? 4 + definition_levels(column, rows) : 0);
}

void payload_bytes(Output &out, const Column &column, size_t rows) {
  if (column.valid) {
    out.little_endian(definition_levels(column, rows), 4);
    definition_levels(column, rows, &out);
  }
  const auto *values = static_cast<const uint8_t *>(column.values);
  for (size_t row = 0; row < rows && out.ok(); ++row) {
    if (!present(column, row))
      continue;
    // memcpy avoids alignment/aliasing assumptions; emit explicitly LE.
    if (column.type == PhysicalType::Int64) {
      uint64_t value;
      memcpy(&value, values + row * column.stride, sizeof(value));
      out.little_endian(value, sizeof(value));
    } else {
      uint32_t value;
      memcpy(&value, values + row * column.stride, sizeof(value));
      out.little_endian(value, sizeof(value));
    }
  }
}

struct MemorySink {
  uint8_t *data = nullptr;
  size_t capacity = 0;
  size_t used = 0;
};
bool memory_sink(void *context, const uint8_t *data, size_t size) {
  auto &memory = *static_cast<MemorySink *>(context);
  if (size > memory.capacity - memory.used)
    return false;
  memcpy(memory.data + memory.used, data, size);
  memory.used += size;
  return true;
}

bool page(Output &out, const Column &column, size_t rows, Workspace &workspace,
          const Compression *compression, uint64_t &uncompressed_size) {
  const size_t definitions = column.valid ? definition_levels(column, rows) : 0;
  const size_t payload = present_count(column, rows) * width(column.type) +
                         (column.valid ? 4 + definitions : 0);
  size_t compressed = payload;
  if (compression) {
    // Both Outputs reuse the staging buffer, never while the other has bytes.
    out.flush();
    if (!out.ok())
      return false;
    MemorySink memory{compression->raw, compression->raw_capacity};
    Output raw(memory_sink, &memory, workspace);
    payload_bytes(raw, column, rows);
    raw.flush();
    if (!raw.ok() || memory.used != payload)
      return false;
    compressed = compression->compress(compression->context, compression->raw,
                                       payload, compression->encoded,
                                       compression->encoded_capacity);
    if (!compressed || compressed > compression->encoded_capacity)
      return false;
  }
  const auto header_start = out.position();
  Struct header(out);
  header.number(1, kI32, 0); // DATA_PAGE
  header.number(2, kI32, payload);
  header.number(3, kI32, compressed);
  header.field(5, kStruct);
  Struct data(out);
  data.number(1, kI32, rows);
  data.number(2, kI32, 0); // PLAIN
  data.number(3, kI32, 3); // RLE definition levels
  data.number(4, kI32, 3); // RLE repetition levels (no bytes: max level 0)
  data.end();
  header.end();
  uncompressed_size = out.position() - header_start + payload;
  if (compression)
    out.bytes(compression->encoded, compressed);
  else
    payload_bytes(out, column, rows);
  return out.ok();
}

void footer(Output &out, const Column *columns, size_t count, size_t rows,
            const Workspace &workspace, const KeyValue *metadata,
            size_t metadata_count, Codec codec) {
  Struct file(out);
  file.number(1, kI32, 1);
  file.list(2, kStruct, count + 1);
  Struct root(out);
  root.string(4, "telemetry");
  root.number(5, kI32, count);
  root.end();
  for (size_t i = 0; i < count; ++i) {
    Struct leaf(out);
    leaf.number(1, kI32, static_cast<uint8_t>(columns[i].type));
    leaf.number(3, kI32, columns[i].valid ? 1 : 0);
    leaf.string(4, columns[i].name);
    leaf.end();
  }
  file.number(3, kI64, rows);
  file.list(4, kStruct, rows ? 1 : 0);
  if (rows) {
    Struct group(out);
    group.list(1, kStruct, count);
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
      const Column &column = columns[i];
      total += workspace.uncompressed_sizes[i];
      Struct chunk(out);
      chunk.number(2, kI64, 0); // metadata lives only in this footer
      chunk.field(3, kStruct);
      Struct meta(out);
      meta.number(1, kI32, static_cast<uint8_t>(column.type));
      meta.list(2, kI32, 2);
      out.integer(0); // PLAIN
      out.integer(3); // RLE
      meta.list(3, kBinary, 1);
      out.string(column.name);
      meta.number(4, kI32, static_cast<uint8_t>(codec));
      meta.number(5, kI64, rows);
      meta.number(6, kI64, workspace.uncompressed_sizes[i]);
      meta.number(7, kI64, workspace.sizes[i]);
      meta.number(9, kI64, workspace.offsets[i]);
      meta.field(12, kStruct); // Statistics: null count, no min/max claims
      Struct stats(out);
      stats.number(3, kI64, rows - present_count(column, rows));
      stats.end();
      meta.end();
      chunk.end();
    }
    group.number(2, kI64, total);
    group.number(3, kI64, rows);
    group.end();
  }
  if (metadata_count) {
    file.list(5, kStruct, metadata_count);
    for (size_t i = 0; i < metadata_count; ++i) {
      Struct kv(out);
      kv.string(1, metadata[i].key);
      kv.string(2, metadata[i].value);
      kv.end();
    }
  }
  file.string(6, "m5stack-aq-parquet version 0.1");
  file.end();
}

bool bounded_string(const char *value, size_t limit, bool allow_empty = false) {
  if (!value || (!allow_empty && !value[0]))
    return false;
  for (size_t i = 0; i <= limit; ++i) {
    if (!value[i])
      return true;
  }
  return false;
}

} // namespace

Result write_parquet(Sink sink, void *context, const Column *columns,
                     size_t column_count, size_t row_count,
                     Workspace &workspace, const KeyValue *metadata,
                     size_t metadata_count, const Compression *compression) {
  if (compression &&
      (compression->codec != Codec::Lz4Raw || !compression->raw ||
       !compression->encoded || !compression->compress ||
       compression->encoded_capacity > INT32_MAX))
    return {false, 0, "invalid compression configuration"};
  if (!sink || !columns || !column_count || column_count > kMaxColumns ||
      row_count > kMaxRows || metadata_count > 64 ||
      (metadata_count && !metadata)) {
    return {false, 0, "invalid argument or capacity exceeded"};
  }
  for (size_t i = 0; i < column_count; ++i) {
    const Column &column = columns[i];
    if (!bounded_string(column.name, 255) ||
        (column.type != PhysicalType::Int32 &&
         column.type != PhysicalType::Int64 &&
         column.type != PhysicalType::Float)) {
      return {false, 0, "invalid column name or type"};
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(column.name, columns[j].name))
        return {false, 0, "duplicate column name"};
    }
    if (row_count &&
        (!column.values || column.stride < width(column.type) ||
         column.stride > SIZE_MAX / row_count ||
         (column.valid && (!column.valid_stride ||
                           column.valid_stride > SIZE_MAX / row_count)))) {
      return {false, 0, "invalid column buffer or stride"};
    }
    if (compression && row_count &&
        payload_size(column, row_count) > compression->raw_capacity)
      return {false, 0, "compression page capacity exceeded"};
  }
  for (size_t i = 0; i < metadata_count; ++i) {
    if (!bounded_string(metadata[i].key, 255) ||
        !bounded_string(metadata[i].value, 4096, true)) {
      return {false, 0, "invalid metadata key or value"};
    }
  }
  Output out(sink, context, workspace);
  out.bytes("PAR1", 4);
  if (row_count) {
    for (size_t i = 0; i < column_count && out.ok(); ++i) {
      workspace.offsets[i] = out.position();
      if (!page(out, columns[i], row_count, workspace, compression,
                workspace.uncompressed_sizes[i]))
        return {false, out.accepted(),
                "page encoding/compression or sink failed"};
      workspace.sizes[i] = out.position() - workspace.offsets[i];
    }
  }
  if (!out.ok())
    return {false, out.accepted(), "sink write failed"};
  const uint64_t footer_start = out.position();
  footer(out, columns, column_count, row_count, workspace, metadata,
         metadata_count,
         compression ? compression->codec : Codec::Uncompressed);
  out.little_endian(out.position() - footer_start, 4);
  out.bytes("PAR1", 4);
  out.flush();
  return {out.ok(), out.accepted(), out.ok() ? nullptr : "sink write failed"};
}

} // namespace telemetry
