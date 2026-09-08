#include "telemetry_logger.h"
#include "ltr553.h"
#include "lz4_codec.h"
#include "parquet_writer.h"
#include "telemetry_contract.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <numeric>
#include <sys/stat.h>
#include <unistd.h>

namespace telemetry {
namespace {
using namespace contract;
constexpr std::size_t kMaxRows = 90;
constexpr std::int64_t kSampleUs = 10000000;
constexpr const char *kDirectory = "/sd/output";
struct WriterState {
  Sample rows[kMaxRows];
  Column columns[field_count];
  Workspace workspace{};
  Lz4Workspace lz4{};
};
struct Command {
  char text[448]{};
  std::int64_t received_mono_us = 0;
};
WriterState *writer_state = nullptr;
QueueHandle_t samples = nullptr;
QueueHandle_t commands = nullptr;
SemaphoreHandle_t spi_mutex = nullptr;
Ltr553 light_sensor;
std::atomic<std::uint32_t> dropped{0}, errors{0}, finalized{0}, buffered{0};
std::atomic<std::uint32_t> write_us{0}, sync_us{0}, queue_peak{0};
std::atomic<std::uint32_t> total_kib{0}, used_kib{0}, rotation_seconds{900};
std::int64_t boot_hi = 0, boot_lo = 0, device = 0, next_sample_us = 0;
std::int64_t sample_sequence = 0, missed_deadlines = 0;
char boot_text[33]{};
char device_text[13]{};
char station_text[37]{};
portMUX_TYPE clock_mutex = portMUX_INITIALIZER_UNLOCKED;
std::int64_t anchor_mono_us = 0, anchor_utc_ns = 0;
std::int32_t clock_generation = 0;
bool mounted = false;
bool accepting = false;
Codec selected_codec = Codec::Uncompressed;
const char *codec_name(Codec codec) {
  return codec == Codec::Lz4Raw ? "LZ4_RAW" : "UNCOMPRESSED";
}

class BusLock {
public:
  BusLock() { lock_display(); }
  ~BusLock() { unlock_display(); }
};

std::uint32_t crc_update(std::uint32_t crc, const std::uint8_t *data,
                         std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

bool sink(void *context, const std::uint8_t *data, std::size_t size) {
  BusLock lock;
  return std::fwrite(data, 1, size, static_cast<FILE *>(context)) == size;
}

bool finalized_file(const char *path, std::uint32_t &size, std::uint32_t &crc) {
  // This is a structural completion check. PyArrow + DuckDB perform
  // conformance.
  FILE *file = nullptr;
  {
    BusLock lock;
    file = std::fopen(path, "rb");
  }
  if (!file)
    return false;
  std::uint8_t block[512];
  std::uint8_t magic[4]{}, tail[8]{};
  bool ok = false;
  {
    BusLock lock;
    if (std::fseek(file, 0, SEEK_END) == 0) {
      const long length = std::ftell(file);
      if (length >= 12 && length <= 1048576) {
        size = static_cast<std::uint32_t>(length);
        ok = std::fseek(file, 0, SEEK_SET) == 0 &&
             std::fread(magic, 1, 4, file) == 4 &&
             std::fseek(file, -8, SEEK_END) == 0 &&
             std::fread(tail, 1, 8, file) == 8;
        const std::uint32_t footer = tail[0] | (std::uint32_t(tail[1]) << 8) |
                                     (std::uint32_t(tail[2]) << 16) |
                                     (std::uint32_t(tail[3]) << 24);
        ok = ok && std::memcmp(magic, "PAR1", 4) == 0 &&
             std::memcmp(tail + 4, "PAR1", 4) == 0 && footer > 0 &&
             footer <= size - 12 && std::fseek(file, 0, SEEK_SET) == 0;
      }
    }
  }
  crc = 0xffffffffU;
  std::uint32_t read_bytes = 0;
  while (ok && read_bytes < size) {
    std::size_t count;
    {
      BusLock lock;
      count = std::fread(block, 1, sizeof(block), file);
    }
    if (!count) {
      ok = false;
      break;
    }
    crc = crc_update(crc, block, count);
    read_bytes += count;
  }
  {
    BusLock lock;
    ok = std::fclose(file) == 0 && ok;
  }
  crc ^= 0xffffffffU;
  return ok && read_bytes == size;
}

void prepare_columns() {
  contract::prepare_columns(writer_state->columns, writer_state->rows);
}

bool station_identity() {
  Preferences settings;
  if (!settings.begin("parquet", false))
    return false;
  if (settings.isKey("station")) {
    settings.getString("station", station_text, sizeof(station_text));
  } else {
    std::uint8_t id[16];
    esp_fill_random(id, sizeof(id));
    id[6] = (id[6] & 0x0f) | 0x40;
    id[8] = (id[8] & 0x3f) | 0x80;
    std::snprintf(
        station_text, sizeof(station_text),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9],
        id[10], id[11], id[12], id[13], id[14], id[15]);
    if (settings.putString("station", station_text) != 36) {
      settings.end();
      return false;
    }
  }
  settings.end();
  if (std::strlen(station_text) != 36)
    return false;
  for (std::size_t i = 0; i < 36; ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? station_text[i] != '-'
             : !((station_text[i] >= '0' && station_text[i] <= '9') ||
                 (station_text[i] >= 'a' && station_text[i] <= 'f')))
      return false;
  }
  return true;
}

bool make_directories(const char *path) {
  char copy[416];
  if (std::strlen(path) >= sizeof(copy))
    return false;
  std::strcpy(copy, path);
  // /sd is the already mounted filesystem; create only descendants.
  for (char *part = copy + 4;; ++part) {
    if (*part != '/' && *part != '\0')
      continue;
    const char saved = *part;
    *part = '\0';
    bool ok;
    {
      BusLock lock;
      ok = ::mkdir(copy, 0700) == 0 || errno == EEXIST;
    }
    *part = saved;
    if (!ok)
      return false;
    if (!saved)
      return true;
  }
}

bool write_batch(std::size_t count, bool benchmark = false,
                 Codec codec = Codec::Uncompressed) {
  static std::uint32_t attempt = 0;
  if (!count)
    return true;
  if (!benchmark)
    codec = selected_codec;
  const auto first = writer_state->rows[0].data[sequence];
  const auto last = writer_state->rows[count - 1].data[sequence];
  char partition[160], prefix[24];
  const Sample &first_row = writer_state->rows[0];
  if (benchmark) {
    std::snprintf(partition, sizeof(partition), "benchmarks/boot=%s",
                  boot_text);
    std::strcpy(prefix, codec == Codec::Lz4Raw ? "lz4_raw" : "uncompressed");
  } else if (first_row.valid[event_time_utc_ns]) {
    const std::int64_t seconds = first_row.data[event_time_utc_ns] / 1000000000;
    const std::time_t window =
        (seconds / rotation_seconds.load()) * rotation_seconds.load();
    std::tm utc{};
    if (!::gmtime_r(&window, &utc)) {
      ++errors;
      return false;
    }
    std::snprintf(partition, sizeof(partition),
                  "station=%s/year=%04d/month=%02d/day=%02d", station_text,
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    std::snprintf(prefix, sizeof(prefix), "data_%02d%02d", utc.tm_hour,
                  utc.tm_min);
  } else {
    std::snprintf(partition, sizeof(partition), "station=%s/unsynced/boot=%s",
                  station_text, boot_text);
    std::strcpy(prefix, "data_unsynced");
  }
  char name[384], path[416], ready[416], directory[192];
  std::snprintf(directory, sizeof(directory), "%s/%s", kDirectory, partition);
  if (!make_directories(directory)) {
    ++errors;
    Serial.println("PARQUET ERROR operation=mkdir");
    return false;
  }
  std::snprintf(name, sizeof(name), "%s/%s_%s_%lld-%lld-%lu", partition, prefix,
                boot_text, static_cast<long long>(first),
                static_cast<long long>(last),
                static_cast<unsigned long>(attempt++));
  std::snprintf(path, sizeof(path), "%s/%s.partial", kDirectory, name);
  std::snprintf(ready, sizeof(ready), "%s/%s.parquet", kDirectory, name);
  const auto started = esp_timer_get_time();
  FILE *file = nullptr;
  {
    BusLock lock;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      file = ::fdopen(fd, "wb");
      if (!file)
        ::close(fd);
    }
  }
  if (!file) {
    ++errors;
    Serial.printf("PARQUET ERROR operation=create errno=%d buffered=%u\n",
                  errno, unsigned(count));
    return false;
  }
  // Internal RAM staging is deliberately distinct from the PSRAM row buffer.
  char staging[4096];
  std::setvbuf(file, staging, _IOFBF, sizeof(staging));
  char interval_text[12];
  std::snprintf(interval_text, sizeof(interval_text), "%lu",
                static_cast<unsigned long>(rotation_seconds.load()));
  const KeyValue metadata[] = {
      {"schema_version", kSchemaName},
      {"device_id", device_text},
      {"station_id", station_text},
      {"boot_id", boot_text},
      {"firmware", kFirmware},
      {"dictionary_version", kSchemaName},
      {"dictionary_uri", kDictionaryUri},
      {"dictionary_sha256", kDictionarySha256},
      {"acquisition_config_id", kConfigurationId},
      {"acquisition_config", kConfiguration},
      {"deployment_id", kUnknown},
      {"calibration_id", kUnknown},
      {"time_semantics", kTimeSemantics},
      {"rotation_interval_s", interval_text},
      {"compression", codec_name(codec)},
      {"purpose", benchmark ? "codec-comparison-duplicate-rows" : "telemetry"},
      {"board", "CoreS3 ESP32-S3 rev0.2"},
      {"sample_interval_ms", "10000"},
      {"clock", "0=unsynchronized/null,1=host estimate; RTC calendar "
                "untrusted; partitions UTC"},
      {"pms_status", "0=missing,1=warming,2=stale,3=sensor-error,4=valid"},
      {"light_status",
       "0=unavailable,1=not-fresh,2=invalid,3=valid,4=io-error"},
      {"proximity_status",
       "0=unavailable,1=not-fresh,2=saturated,3=valid,4=io-error"},
      {"magnetic_units", "M5Unified BMI270 auxiliary raw counts; uncalibrated"},
      {"unavailable", "SHT20 address collision; battery current unsupported; "
                      "camera/audio not sampled"},
      {"durability",
       "RAM batch; unfinished rows lost on reset; completed files retained"}};
  writer_state->lz4.codec_us = 0;
  auto compression = writer_state->lz4.configuration();
  const auto writer_started = esp_timer_get_time();
  const auto result = write_parquet(
      sink, file, writer_state->columns, field_count, count,
      writer_state->workspace, metadata, sizeof(metadata) / sizeof(metadata[0]),
      codec == Codec::Lz4Raw ? &compression : nullptr);
  const auto writer_elapsed = esp_timer_get_time() - writer_started;
  bool ok = result.ok;
  const auto sync_started = esp_timer_get_time();
  {
    BusLock lock;
    if (std::fflush(file) != 0)
      ok = false;
    if (::fsync(::fileno(file)) != 0)
      ok = false;
    if (std::fclose(file) != 0)
      ok = false;
  }
  sync_us = static_cast<std::uint32_t>(esp_timer_get_time() - sync_started);
  std::uint32_t size = 0, crc = 0;
  if (ok)
    ok = finalized_file(path, size, crc) && size == result.bytes_written;
  if (ok) {
    BusLock lock;
    ok = ::access(ready, F_OK) != 0 && ::rename(path, ready) == 0;
    used_kib = SD.usedBytes() / 1024;
  }
  write_us = static_cast<std::uint32_t>(esp_timer_get_time() - started);
  if (!ok) {
    ++errors;
    Serial.printf("PARQUET ERROR operation=write file=%s reason=%s errno=%d "
                  "buffered=%u\n",
                  name, result.error ? result.error : "io-or-validation", errno,
                  unsigned(count));
    return false;
  }
  ++finalized;
  Serial.printf(
      "PARQUET READY name=%s.parquet rows=%u first=%lld last=%lld "
      "bytes=%lu crc32=%08lx write_us=%lu sync_us=%lu heap_free=%lu "
      "psram_free=%lu stack_free=%u codec=%s codec_us=%llu "
      "writer_us=%lld codec_workspace_bytes=%u heap_min=%lu\n",
      name, unsigned(count), static_cast<long long>(first),
      static_cast<long long>(last), static_cast<unsigned long>(size),
      static_cast<unsigned long>(crc),
      static_cast<unsigned long>(write_us.load()),
      static_cast<unsigned long>(sync_us.load()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getFreePsram()),
      unsigned(uxTaskGetStackHighWaterMark(nullptr)), codec_name(codec),
      static_cast<unsigned long long>(writer_state->lz4.codec_us),
      static_cast<long long>(writer_elapsed), unsigned(sizeof(Lz4Workspace)),
      static_cast<unsigned long>(ESP.getMinFreeHeap()));
  return true;
}

bool safe_name(const char *name) {
  const std::size_t length = std::strlen(name);
  if (length < 9 || length > 384 || name[0] == '/' ||
      std::strcmp(name + length - 8, ".parquet") != 0)
    return false;
  for (std::size_t i = 0; i < length; ++i)
    if (!((name[i] >= '0' && name[i] <= '9') ||
          (name[i] >= 'a' && name[i] <= 'z') || name[i] == '-' ||
          name[i] == '.' || name[i] == '_' || name[i] == '=' || name[i] == '/'))
      return false;
  return std::strstr(name, "..") == nullptr &&
         std::strstr(name, "//") == nullptr;
}

void list_directory(const char *relative, unsigned depth, unsigned &partials,
                    const char *base = kDirectory,
                    const char *export_prefix = "") {
  if (depth > 6)
    return;
  char directory_path[416];
  std::snprintf(directory_path, sizeof(directory_path), "%s/%s", base,
                relative);
  DIR *directory;
  {
    BusLock lock;
    directory = ::opendir(directory_path);
  }
  if (!directory) {
    Serial.println("PARQUET ERROR operation=list");
    return;
  }
  for (;;) {
    const struct dirent *entry;
    {
      BusLock lock;
      entry = ::readdir(directory);
    }
    if (!entry)
      break;
    if (entry->d_name[0] == '.')
      continue;
    char name[384], path[416];
    const int length = std::snprintf(name, sizeof(name), "%s%s%s", relative,
                                     *relative ? "/" : "", entry->d_name);
    if (length < 0 || std::size_t(length) >= sizeof(name))
      continue;
    std::snprintf(path, sizeof(path), "%s/%s", base, name);
    struct stat info{};
    int stat_result;
    {
      BusLock lock;
      stat_result = ::stat(path, &info);
    }
    if (stat_result != 0)
      continue;
    if (S_ISDIR(info.st_mode))
      list_directory(name, depth + 1, partials, base, export_prefix);
    else if (safe_name(name))
      Serial.printf("PARQUET FILE name=%s%s bytes=%lu\n", export_prefix, name,
                    static_cast<unsigned long>(info.st_size));
    else if (std::strstr(name, ".partial"))
      ++partials;
  }
  {
    BusLock lock;
    ::closedir(directory);
  }
}

void list_files() {
  unsigned partials = 0;
  list_directory("", 0, partials);
  struct stat legacy{};
  bool has_legacy;
  {
    BusLock lock;
    has_legacy = ::stat("/sd/parquet", &legacy) == 0 && S_ISDIR(legacy.st_mode);
  }
  if (has_legacy)
    list_directory("", 0, partials, "/sd/parquet", "legacy-parquet/");
  Serial.printf("PARQUET PARTIAL retained=%u recovery=not-implemented\n",
                partials);
  Serial.println("PARQUET LIST END");
}

void send_file(const char *name) {
  if (!safe_name(name)) {
    Serial.println("PARQUET ERROR operation=get reason=invalid-name");
    return;
  }
  char path[416];
  if (std::strncmp(name, "legacy-parquet/", 15) == 0)
    std::snprintf(path, sizeof(path), "/sd/parquet/%.369s", name + 15);
  else
    std::snprintf(path, sizeof(path), "%s/%.384s", kDirectory, name);
  std::uint32_t size = 0, expected_crc = 0;
  if (!finalized_file(path, size, expected_crc)) {
    Serial.println("PARQUET ERROR operation=get reason=invalid-file");
    return;
  }
  FILE *file;
  {
    BusLock lock;
    file = std::fopen(path, "rb");
  }
  if (!file) {
    Serial.println("PARQUET ERROR operation=get reason=open");
    return;
  }
  Serial.printf("PARQUET DATA BEGIN name=%s bytes=%lu\n", name,
                static_cast<unsigned long>(size));
  std::uint8_t block[256];
  char hex[513];
  constexpr char digits[] = "0123456789abcdef";
  std::uint32_t offset = 0;
  while (offset < size) {
    std::size_t count;
    {
      BusLock lock;
      count = std::fread(block, 1, sizeof(block), file);
    }
    if (!count)
      break;
    for (std::size_t i = 0; i < count; ++i) {
      hex[2 * i] = digits[block[i] >> 4];
      hex[2 * i + 1] = digits[block[i] & 15];
    }
    hex[2 * count] = '\0';
    Serial.printf("PARQUET DATA offset=%lu hex=%s\n",
                  static_cast<unsigned long>(offset), hex);
    offset += count;
    delay(1);
  }
  {
    BusLock lock;
    std::fclose(file);
  }
  Serial.printf("PARQUET DATA END name=%s bytes=%lu crc32=%08lx\n", name,
                static_cast<unsigned long>(offset),
                static_cast<unsigned long>(expected_crc));
}

void storage_worker(void *) {
  bool storage_ready;
  {
    BusLock lock;
    storage_ready =
        mounted && (::mkdir(kDirectory, 0700) == 0 || errno == EEXIST);
  }
  if (storage_ready) {
    {
      BusLock lock;
      total_kib = SD.totalBytes() / 1024;
      used_kib = SD.usedBytes() / 1024;
    }
    list_files();
  } else {
    ++errors;
    Serial.println("PARQUET ERROR operation=mount-or-directory "
                   "samples_will_be_dropped=true");
  }
  std::size_t count = 0;
  bool failed = false;
  Sample row;
  Command command;
  for (;;) {
    if (xQueueReceive(samples, &row, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (!storage_ready || failed || count == kMaxRows) {
        ++dropped;
      } else {
        // Never mix clock epochs or UTC windows (including midnight) in a file.
        if (count) {
          const auto &previous = writer_state->rows[0];
          const auto window_ns =
              std::int64_t(rotation_seconds.load()) * 1000000000;
          const bool different_window =
              row.valid[event_time_utc_ns] &&
              previous.valid[event_time_utc_ns] &&
              row.data[event_time_utc_ns] / window_ns !=
                  previous.data[event_time_utc_ns] / window_ns;
          if (row.data[clock_epoch] != previous.data[clock_epoch] ||
              different_window) {
            if (write_batch(count)) {
              count = 0;
              buffered = 0;
            } else {
              failed = true;
              ++dropped;
              continue;
            }
          }
        }
        writer_state->rows[count++] = row;
        buffered = count;
        if (count >= rotation_seconds.load() / 10) {
          if (write_batch(count)) {
            count = 0;
            buffered = 0;
          } else
            failed = true;
        }
      }
    }
    if (xQueueReceive(commands, &command, 0) != pdTRUE)
      continue;
    if (std::strcmp(command.text, "parquet status") == 0) {
      Serial.printf(
          "PARQUET STATUS interval_s=%lu buffered=%u finalized=%lu "
          "dropped=%lu errors=%lu queue_peak=%lu failed=%s station=%s "
          "codec=%s schema=%s config=%s deployment=unknown "
          "calibration=unknown\n",
          static_cast<unsigned long>(rotation_seconds.load()), unsigned(count),
          static_cast<unsigned long>(finalized.load()),
          static_cast<unsigned long>(dropped.load()),
          static_cast<unsigned long>(errors.load()),
          static_cast<unsigned long>(queue_peak.load()),
          failed ? "true" : "false", station_text, codec_name(selected_codec),
          kSchemaName, kConfigurationId);
    } else if (std::strcmp(command.text, "parquet schema") == 0) {
      Serial.printf("PARQUET SCHEMA BEGIN schema=%s columns=%u sha256=%s\n",
                    kSchemaName, unsigned(field_count), kDictionarySha256);
      for (const auto &field : kFields) {
        Serial.printf("PARQUET FIELD name=%s type=%u procedure=%s unit=%s "
                      "validity=%s property=%s\n",
                      field.name, unsigned(field.type), field.procedure,
                      field.unit, field.validity, field.property_uri);
        delay(1);
      }
      Serial.println("PARQUET SCHEMA END");
    } else if (std::strcmp(command.text, "parquet codec-test") == 0) {
      if (!count || !storage_ready || failed) {
        Serial.println("PARQUET ERROR operation=codec-test "
                       "reason=empty-or-storage-failed");
      } else if (write_batch(count, true, Codec::Uncompressed) &&
                 write_batch(count, true, Codec::Lz4Raw)) {
        // Keep the original batch for normal telemetry rotation. Benchmark
        // copies live outside station trees and are explicitly labeled.
        Serial.printf("PARQUET BENCH END rows=%u retained_for_telemetry=true\n",
                      unsigned(count));
      }
    } else if (std::strcmp(command.text, "parquet codec none") == 0 ||
               std::strcmp(command.text, "parquet codec lz4") == 0) {
      if (count && !write_batch(count)) {
        failed = true;
        continue;
      }
      count = 0;
      buffered = 0;
      failed = false;
      selected_codec =
          command.text[14] == 'l' ? Codec::Lz4Raw : Codec::Uncompressed;
      Serial.printf("PARQUET CONFIG codec=%s persistent=false\n",
                    codec_name(selected_codec));
    } else if (std::strcmp(command.text, "parquet flush") == 0) {
      if (count && storage_ready) {
        if (write_batch(count)) {
          count = 0;
          buffered = 0;
          failed = false;
        }
      } else
        Serial.println(
            "PARQUET ERROR operation=flush reason=empty-or-no-storage");
    } else if (std::strcmp(command.text, "parquet interval 600") == 0 ||
               std::strcmp(command.text, "parquet interval 900") == 0) {
      if (count && !write_batch(count)) {
        failed = true;
        continue;
      }
      count = 0;
      buffered = 0;
      failed = false;
      rotation_seconds = command.text[17] == '6' ? 600 : 900;
      Serial.printf("PARQUET CONFIG interval_s=%lu persistent=false\n",
                    static_cast<unsigned long>(rotation_seconds.load()));
    } else if (std::strncmp(command.text, "parquet time ", 13) == 0) {
      char *end = nullptr;
      const auto seconds = std::strtoll(command.text + 13, &end, 10);
      if (seconds < 1577836800LL || seconds > 4102444800LL || !end || *end) {
        Serial.println("PARQUET ERROR operation=time reason=invalid-epoch");
        continue;
      }
      const auto mono = command.received_mono_us;
      portENTER_CRITICAL(&clock_mutex);
      anchor_mono_us = mono;
      anchor_utc_ns = seconds * 1000000000;
      ++clock_generation;
      portEXIT_CRITICAL(&clock_mutex);
      Serial.printf("PARQUET TIME epoch_s=%lld source=host monotonic_us=%lld\n",
                    static_cast<long long>(seconds),
                    static_cast<long long>(mono));
    } else if (std::strcmp(command.text, "parquet list") == 0)
      list_files();
    else if (std::strncmp(command.text, "parquet get ", 12) == 0)
      send_file(command.text + 12);
    else
      Serial.println("PARQUET ERROR operation=command reason=unknown-command");
  }
}

void collect(const PmsSnapshot &pms, std::int64_t now, std::int64_t scheduled) {
  Sample row;
  row.integer(schema_version, kSchemaVersion);
  row.counter(device_id, device);
  row.counter(boot_id_hi, boot_hi);
  row.counter(boot_id_lo, boot_lo);
  row.counter(sequence, sample_sequence++);
  row.counter(monotonic_us, now);
  row.counter(scheduled_us, scheduled);
  row.counter(sample_jitter_us, now - scheduled);
  std::int64_t utc_anchor, mono_anchor;
  std::int32_t generation;
  portENTER_CRITICAL(&clock_mutex);
  utc_anchor = anchor_utc_ns;
  mono_anchor = anchor_mono_us;
  generation = clock_generation;
  portEXIT_CRITICAL(&clock_mutex);
  apply_clock(row, now, mono_anchor, utc_anchor, generation);
  const int status = !pms.present        ? 0
                     : now < 30000000    ? 1
                     : pms.age_ms > 5000 ? 2
                     : pms.error         ? 3
                                         : 4;
  row.integer(pms_status, status);
  if (pms.present) {
    row.counter(pms_age_ms, pms.age_ms);
    row.counter(pms_received_mono_us, pms.received_mono_us);
    row.integer(pms_firmware, pms.firmware);
    row.integer(pms_error, pms.error);
  }
  if (status == 4)
    for (std::size_t i = 0; i < 12; ++i)
      row.integer(static_cast<Field>(pm1_cf1_ug_m3 + i), pms.values[i]);
  row.counter(pms_frames, pms.frames);
  row.counter(pms_checksum_errors, pms.checksum_errors);
  row.counter(pms_length_errors, pms.length_errors);
  const auto fresh =
      M5.Imu.isEnabled() ? M5.Imu.update() : m5::IMU_Class::sensor_mask_none;
  row.integer(imu_fresh_mask, fresh);
  m5::IMU_Class::imu_data_t imu{};
  M5.Imu.getImuData(&imu);
  if (fresh & m5::IMU_Class::sensor_mask_accel) {
    row.number(accel_x_g, imu.accel.x);
    row.number(accel_y_g, imu.accel.y);
    row.number(accel_z_g, imu.accel.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_gyro) {
    row.number(gyro_x_dps, imu.gyro.x);
    row.number(gyro_y_dps, imu.gyro.y);
    row.number(gyro_z_dps, imu.gyro.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_mag) {
    row.integer(mag_x_raw, M5.Imu.getRawData(6));
    row.integer(mag_y_raw, M5.Imu.getRawData(7));
    row.integer(mag_z_raw, M5.Imu.getRawData(8));
  }
  float temperature;
  if (M5.Imu.isEnabled() && M5.Imu.getTemp(&temperature))
    row.number(imu_temperature_c, temperature);
  const auto light = light_sensor.read();
  row.integer(light_status, !light_sensor.available() ? 0
                            : light.io_error          ? 4
                            : !light.als_fresh        ? 1
                            : light.als_valid         ? 3
                                                      : 2);
  row.integer(proximity_status, !light_sensor.available() ? 0
                                : light.io_error          ? 4
                                : !light.proximity_fresh  ? 1
                                : light.proximity_valid   ? 3
                                                          : 2);
  if (light.als_valid) {
    row.integer(light_ch0_raw, light.als_ch0);
    row.integer(light_ch1_raw, light.als_ch1);
  }
  if (light.proximity_valid)
    row.integer(proximity_raw, light.proximity);
  row.integer(vbus_mv, M5.Power.getVBUSVoltage());
  row.integer(battery_mv, M5.Power.getBatteryVoltage());
  const int battery = M5.Power.getBatteryLevel();
  if (battery >= 0 && battery <= 100)
    row.integer(battery_percent, battery);
  row.integer(charging_status, M5.Power.isCharging());
  // CoreS3 getBatteryCurrent() is an unsupported constant zero. Leave null.
  row.integer(external_5v_enabled, M5.Power.getExtOutput());
  row.integer(usb_output_enabled, M5.Power.getUsbOutput());
  m5::rtc_datetime_t rtc{};
  const bool rtc_ok = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&rtc);
  row.integer(rtc_read_ok, rtc_ok);
  if (rtc_ok) {
    row.integer(rtc_date_yyyymmdd,
                rtc.date.year * 10000 + rtc.date.month * 100 + rtc.date.date);
    row.integer(rtc_time_hhmmss, rtc.time.hours * 10000 +
                                     rtc.time.minutes * 100 + rtc.time.seconds);
  }
  row.integer(touch_points, M5.Touch.getCount());
  if (M5.Touch.getCount()) {
    const auto &touch = M5.Touch.getDetail();
    row.integer(touch_x_px, touch.x);
    row.integer(touch_y_px, touch.y);
  }
  row.counter(heap_free_bytes, ESP.getFreeHeap());
  row.counter(heap_min_free_bytes, ESP.getMinFreeHeap());
  row.counter(psram_free_bytes, ESP.getFreePsram());
  if (mounted) {
    row.counter(sd_total_bytes, std::int64_t(total_kib.load()) * 1024);
    row.counter(sd_used_bytes, std::int64_t(used_kib.load()) * 1024);
  }
  row.counter(rows_dropped, dropped.load());
  row.counter(sample_deadlines_missed, missed_deadlines);
  row.counter(storage_errors, errors.load());
  row.counter(files_finalized, finalized.load());
  row.counter(last_write_us, write_us.load());
  row.counter(last_sync_us, sync_us.load());
  row.integer(queue_high_water, queue_peak.load());
  row.counter(collection_completed_mono_us, esp_timer_get_time());
  if (xQueueSend(samples, &row, 0) != pdTRUE)
    ++dropped;
  const auto depth =
      static_cast<std::uint32_t>(uxQueueMessagesWaiting(samples));
  if (depth > queue_peak.load())
    queue_peak = depth;
  Serial.printf("PARQUET ROW sequence=%lld monotonic_us=%lld jitter_us=%lld "
                "pms_status=%d imu_mask=%u light_valid=%s proximity_valid=%s "
                "buffered=%lu dropped=%lu\n",
                static_cast<long long>(row.data[sequence]),
                static_cast<long long>(now),
                static_cast<long long>(now - scheduled), status,
                unsigned(fresh), light.als_valid ? "true" : "false",
                light.proximity_valid ? "true" : "false",
                static_cast<unsigned long>(buffered.load()),
                static_cast<unsigned long>(dropped.load()));
}
} // namespace

void lock_display() {
  if (spi_mutex)
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
}
void unlock_display() {
  if (spi_mutex)
    xSemaphoreGive(spi_mutex);
}

void begin_logger(bool sd_mounted) {
  mounted = sd_mounted;
  if (!station_identity()) {
    Serial.println("PARQUET ERROR operation=station-identity logging=false");
    return;
  }
  std::uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    Serial.println("PARQUET ERROR operation=identity");
    return;
  }
  device = std::accumulate(std::begin(mac), std::end(mac), std::int64_t{0},
                           [](std::int64_t value, std::uint8_t byte) {
                             return (value << 8) | byte;
                           });
  std::snprintf(device_text, sizeof(device_text), "%012llx",
                static_cast<unsigned long long>(device));
  std::uint32_t random[4];
  esp_fill_random(random, sizeof(random));
  std::memcpy(&boot_hi, random, 8);
  std::memcpy(&boot_lo, random + 2, 8);
  std::snprintf(boot_text, sizeof(boot_text), "%016llx%016llx",
                static_cast<unsigned long long>(boot_hi),
                static_cast<unsigned long long>(boot_lo));
  spi_mutex = xSemaphoreCreateMutex();
  samples = xQueueCreate(8, sizeof(Sample));
  commands = xQueueCreate(4, sizeof(Command));
  writer_state = static_cast<WriterState *>(heap_caps_calloc(
      1, sizeof(WriterState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!spi_mutex || !samples || !commands || !writer_state) {
    Serial.println("PARQUET ERROR operation=allocate logging=false");
    return;
  }
  new (writer_state) WriterState{};
  prepare_columns();
  const bool light = light_sensor.begin();
  next_sample_us = esp_timer_get_time() + kSampleUs;
  if (xTaskCreate(storage_worker, "parquet-sd", 16384, nullptr, 1, nullptr) !=
      pdPASS) {
    Serial.println("PARQUET ERROR operation=task logging=false");
    return;
  }
  accepting = true;
  Serial.printf(
      "PARQUET BEGIN schema=%s columns=%u sample_s=10 "
      "interval_s=900 max_rows=90 psram_workspace_bytes=%u row_bytes=%u "
      "boot=%s station=%s light_available=%s codec=UNCOMPRESSED\n",
      kSchemaName, unsigned(field_count), unsigned(sizeof(WriterState)),
      unsigned(sizeof(Sample)), boot_text, station_text,
      light ? "true" : "false");
}

void poll_logger(const PmsSnapshot &pms) {
  if (!accepting)
    return;
  const auto now = esp_timer_get_time();
  if (now >= next_sample_us) {
    const auto skipped = (now - next_sample_us) / kSampleUs;
    missed_deadlines += skipped;
    sample_sequence += skipped;
    next_sample_us += skipped * kSampleUs;
    collect(pms, now, next_sample_us);
    next_sample_us += kSampleUs;
  }
  static Command input{};
  static std::size_t length = 0;
  static bool overflow = false;
  for (unsigned limit = 0; limit < 128 && Serial.available(); ++limit) {
    const int byte = Serial.read();
    if (byte == '\r')
      continue;
    if (byte == '\n') {
      input.text[length] = '\0';
      input.received_mono_us = esp_timer_get_time();
      if (overflow || xQueueSend(commands, &input, 0) != pdTRUE)
        Serial.println(
            "PARQUET ERROR operation=command reason=too-long-or-busy");
      length = 0;
      overflow = false;
    } else if (byte >= 32 && byte <= 126) {
      if (length + 1 < sizeof(input.text))
        input.text[length++] = static_cast<char>(byte);
      else
        overflow = true;
    }
  }
}
} // namespace telemetry
