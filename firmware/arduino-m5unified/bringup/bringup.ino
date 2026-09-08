#include <SD.h>
#include <SPI.h>

#include <M5Unified.h>

#include <Arduino.h>
#include <Esp.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>

namespace {

constexpr std::uint32_t kSerialBaud = 115200;
constexpr std::uint32_t kI2cScanFrequency = 100000;
constexpr std::uint32_t kPmsProbeDurationMs = 5000;
constexpr std::uint32_t kDisplayIntervalMs = 10000;
constexpr std::uint8_t kScreenPageCount = 3;
constexpr int kMinimumSwipeDistance = 40;
constexpr int kSdSck = 36;
constexpr int kSdMiso = 35;
constexpr int kSdMosi = 37;
constexpr int kSdCs = 4;
constexpr int kPmsRx = 18;
constexpr int kPmsTx = 17;

HardwareSerial pms_serial(1);

struct PmsFrame {
  std::uint16_t cf1_pm1 = 0;
  std::uint16_t cf1_pm25 = 0;
  std::uint16_t cf1_pm10 = 0;
  std::uint16_t atmospheric_pm1 = 0;
  std::uint16_t atmospheric_pm25 = 0;
  std::uint16_t atmospheric_pm10 = 0;
  std::array<std::uint16_t, 6> particle_counts{};
  std::uint8_t firmware_version = 0;
  std::uint8_t sensor_error = 0;
};

class PmsParser {
public:
  bool push(std::uint8_t byte, PmsFrame &frame) {
    if (used_ == 0 && byte != 0x42) {
      return false;
    }

    buffer_[used_++] = byte;
    if (used_ == 2 && buffer_[1] != 0x4d) {
      resynchronize();
      return false;
    }
    if (used_ == 4 && word_at(2) != 28) {
      ++length_failures_;
      resynchronize();
      return false;
    }
    if (used_ < buffer_.size()) {
      return false;
    }

    const auto sum = std::accumulate(
        buffer_.begin(), buffer_.end() - 2, std::uint16_t{0},
        [](std::uint16_t accumulated, std::uint8_t value) {
          return static_cast<std::uint16_t>(accumulated + value);
        });
    if (sum != word_at(30)) {
      ++checksum_failures_;
      resynchronize();
      return false;
    }

    frame.cf1_pm1 = word_at(4);
    frame.cf1_pm25 = word_at(6);
    frame.cf1_pm10 = word_at(8);
    frame.atmospheric_pm1 = word_at(10);
    frame.atmospheric_pm25 = word_at(12);
    frame.atmospheric_pm10 = word_at(14);
    for (std::size_t index = 0; index < frame.particle_counts.size(); ++index) {
      frame.particle_counts[index] = word_at(16 + index * 2);
    }
    frame.firmware_version = buffer_[28];
    frame.sensor_error = buffer_[29];
    used_ = 0;
    return true;
  }

  std::uint32_t checksum_failures() const { return checksum_failures_; }
  std::uint32_t length_failures() const { return length_failures_; }

private:
  std::uint16_t word_at(std::size_t offset) const {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(buffer_[offset]) << 8) |
        buffer_[offset + 1]);
  }

  void resynchronize() {
    std::size_t next = used_;
    for (std::size_t index = 1; index + 1 < used_; ++index) {
      if (buffer_[index] == 0x42 && buffer_[index + 1] == 0x4d) {
        next = index;
        break;
      }
    }
    if (next == used_ && used_ != 0 && buffer_[used_ - 1] == 0x42) {
      buffer_[0] = 0x42;
      used_ = 1;
      return;
    }
    if (next < used_) {
      const std::size_t remaining = used_ - next;
      std::memmove(buffer_.data(), buffer_.data() + next, remaining);
      used_ = remaining;
      return;
    }
    used_ = 0;
  }

  std::array<std::uint8_t, 32> buffer_{};
  std::size_t used_ = 0;
  std::uint32_t checksum_failures_ = 0;
  std::uint32_t length_failures_ = 0;
};

PmsParser pms_parser;
PmsFrame latest_pms_frame{};
bool has_pms_frame = false;
bool sd_mounted = false;
std::uint32_t latest_pms_ms = 0;
std::uint32_t pms_frame_count = 0;
std::uint32_t last_display_ms = 0;
std::uint8_t screen_page = 0;

const char *board_name(m5::board_t board) {
  switch (board) {
  case m5::board_t::board_M5StackCoreS3:
    return "M5Stack CoreS3";
  case m5::board_t::board_M5StackCoreS3SE:
    return "M5Stack CoreS3 SE";
  default:
    return "unexpected board";
  }
}

const char *flash_mode_name(FlashMode_t mode) {
  switch (mode) {
  case FM_QIO:
    return "qio";
  case FM_QOUT:
    return "qout";
  case FM_DIO:
    return "dio";
  case FM_DOUT:
    return "dout";
  case FM_FAST_READ:
    return "fast-read";
  case FM_SLOW_READ:
    return "slow-read";
  default:
    return "unknown";
  }
}

const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external-pin";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt-watchdog";
  case ESP_RST_TASK_WDT:
    return "task-watchdog";
  case ESP_RST_WDT:
    return "other-watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep-sleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  case ESP_RST_USB:
    return "usb";
  case ESP_RST_JTAG:
    return "jtag";
  case ESP_RST_EFUSE:
    return "efuse";
  case ESP_RST_PWR_GLITCH:
    return "power-glitch";
  case ESP_RST_CPU_LOCKUP:
    return "cpu-lockup";
  default:
    return "unknown";
  }
}

const char *charging_name(m5::Power_Class::is_charging_t charging) {
  switch (charging) {
  case m5::Power_Class::is_charging:
    return "charging";
  case m5::Power_Class::is_discharging:
    return "discharging";
  case m5::Power_Class::charge_unknown:
  default:
    return "unknown";
  }
}

const char *i2c_device_name(std::uint8_t address) {
  switch (address) {
  case 0x21:
    return "GC0308-camera";
  case 0x23:
    return "LTR553-proximity";
  case 0x34:
    return "AXP2101-power";
  case 0x36:
    return "AW88298-amplifier";
  case 0x38:
    return "FT6336U-touch";
  case 0x40:
    return "ES7210-codec-or-SHT20-address-collision";
  case 0x51:
    return "BM8563-RTC";
  case 0x58:
    return "AW9523B-expander";
  case 0x69:
    return "BMI270-IMU";
  default:
    return "unexpected";
  }
}

void report_chip() {
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  std::uint8_t mac[6]{};
  const esp_err_t mac_result = esp_read_mac(mac, ESP_MAC_WIFI_STA);

  Serial.printf(
      "DIAG chip model=%s revision=%u cores=%u features=0x%08lx reset=%s(%d) "
      "board=%s(%d)\n",
      ESP.getChipModel(), static_cast<unsigned>(chip.revision),
      static_cast<unsigned>(chip.cores),
      static_cast<unsigned long>(chip.features),
      reset_reason_name(esp_reset_reason()),
      static_cast<int>(esp_reset_reason()), board_name(M5.getBoard()),
      static_cast<int>(M5.getBoard()));
  if (mac_result == ESP_OK) {
    Serial.printf("DIAG identity wifi_sta_mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    Serial.printf("DIAG identity wifi_sta_mac=unavailable error=%d\n",
                  mac_result);
  }
  Serial.printf("DIAG flash bytes=%lu speed_hz=%lu mode=%s\n",
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getFlashChipSpeed()),
                flash_mode_name(ESP.getFlashChipMode()));
}

void report_memory() {
  Serial.printf(
      "DIAG memory heap_total=%lu heap_free=%lu heap_min_free=%lu "
      "heap_largest_internal=%lu psram_found=%s psram_total=%lu psram_free=%lu "
      "psram_largest=%lu\n",
      static_cast<unsigned long>(ESP.getHeapSize()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      psramFound() ? "true" : "false",
      static_cast<unsigned long>(ESP.getPsramSize()),
      static_cast<unsigned long>(ESP.getFreePsram()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void report_i2c() {
  unsigned found = 0;
  unsigned unexpected = 0;
  // M5Unified excludes reserved addresses 0x00-0x07 and 0x78-0x7f because
  // probing the low range can stop the ESP32-S3 I2C controller.
  for (std::uint8_t address = 8; address < 0x78; ++address) {
    if (!M5.In_I2C.scanID(address, kI2cScanFrequency)) {
      continue;
    }
    ++found;
    const char *name = i2c_device_name(address);
    if (std::strcmp(name, "unexpected") == 0) {
      ++unexpected;
    }
    Serial.printf("DIAG i2c address=0x%02x device=%s\n", address, name);
  }
  Serial.printf(
      "DIAG i2c_summary sda=12 scl=11 frequency_hz=%lu found=%u unexpected=%u "
      "note_bmm150=behind_bmi270_aux_bus\n",
      static_cast<unsigned long>(kI2cScanFrequency), found, unexpected);
}

void report_power() {
  const auto charging = M5.Power.isCharging();
  Serial.printf(
      "DIAG power pmic_type=%d external_5v=%s usb_output=%s vbus_mv=%d "
      "battery_mv=%d battery_percent=%ld charging=%s battery_current_ma=%ld\n",
      static_cast<int>(M5.Power.getType()),
      M5.Power.getExtOutput() ? "on" : "off",
      M5.Power.getUsbOutput() ? "source" : "input",
      static_cast<int>(M5.Power.getVBUSVoltage()),
      static_cast<int>(M5.Power.getBatteryVoltage()),
      static_cast<long>(M5.Power.getBatteryLevel()), charging_name(charging),
      static_cast<long>(M5.Power.getBatteryCurrent()));
}

void report_rtc_imu_touch() {
  m5::rtc_datetime_t datetime{};
  if (M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&datetime)) {
    Serial.printf(
        "DIAG rtc enabled=true date=%04d-%02d-%02d time=%02d:%02d:%02d\n",
        datetime.date.year, datetime.date.month, datetime.date.date,
        datetime.time.hours, datetime.time.minutes, datetime.time.seconds);
  } else {
    Serial.println("DIAG rtc enabled=false_or_read_failed");
  }

  M5.Imu.update();
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  const bool accel_ok = M5.Imu.isEnabled() && M5.Imu.getAccel(&ax, &ay, &az);
  const bool gyro_ok = M5.Imu.isEnabled() && M5.Imu.getGyro(&gx, &gy, &gz);
  Serial.printf(
      "DIAG imu enabled=%s type=%d accel_ok=%s accel_g=%.4f,%.4f,%.4f "
      "gyro_ok=%s gyro_dps=%.4f,%.4f,%.4f\n",
      M5.Imu.isEnabled() ? "true" : "false", static_cast<int>(M5.Imu.getType()),
      accel_ok ? "true" : "false", ax, ay, az, gyro_ok ? "true" : "false", gx,
      gy, gz);

  M5.update();
  Serial.printf("DIAG touch controller_ack=%s active_points=%u\n",
                M5.In_I2C.scanID(0x38, kI2cScanFrequency) ? "true" : "false",
                static_cast<unsigned>(M5.Touch.getCount()));
}

bool report_sd() {
  SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  if (!SD.begin(kSdCs, SPI, 25000000, "/sd", 5, false)) {
    Serial.println("DIAG sd mounted=false status=missing_or_mount_failed "
                   "format_attempted=false");
    return false;
  }

  const std::uint8_t type = SD.cardType();
  Serial.printf("DIAG sd mounted=true type=%u card_bytes=%llu total_bytes=%llu "
                "used_bytes=%llu "
                "clock_hz=25000000\n",
                static_cast<unsigned>(type),
                static_cast<unsigned long long>(SD.cardSize()),
                static_cast<unsigned long long>(SD.totalBytes()),
                static_cast<unsigned long long>(SD.usedBytes()));
  return true;
}

bool start_pms() {
  pms_serial.begin(9600, SERIAL_8N1, kPmsRx, kPmsTx);
  const std::uint32_t started = millis();
  bool received = false;

  while (static_cast<std::uint32_t>(millis() - started) < kPmsProbeDurationMs) {
    while (pms_serial.available() > 0) {
      const int value = pms_serial.read();
      if (value >= 0 &&
          pms_parser.push(static_cast<std::uint8_t>(value), latest_pms_frame)) {
        has_pms_frame = true;
        latest_pms_ms = millis();
        ++pms_frame_count;
        received = true;
        break;
      }
    }
    if (received) {
      break;
    }
    delay(1);
  }
  if (!received) {
    Serial.printf(
        "DIAG pmsa003 status=no_valid_frame_within_timeout timeout_ms=%lu "
        "checksum_failures=%lu length_failures=%lu values_valid=false\n",
        static_cast<unsigned long>(kPmsProbeDurationMs),
        static_cast<unsigned long>(pms_parser.checksum_failures()),
        static_cast<unsigned long>(pms_parser.length_failures()));
    return false;
  }

  Serial.printf(
      "DIAG pmsa003 status=frame_received values_valid=%s sensor_error=%u "
      "firmware=%u atmospheric_pm1_ug_m3=%u atmospheric_pm25_ug_m3=%u "
      "atmospheric_pm10_ug_m3=%u cf1_pm1_ug_m3=%u cf1_pm25_ug_m3=%u "
      "cf1_pm10_ug_m3=%u counts_per_0_1l=%u,%u,%u,%u,%u,%u\n",
      latest_pms_frame.sensor_error == 0 ? "true" : "false",
      latest_pms_frame.sensor_error, latest_pms_frame.firmware_version,
      latest_pms_frame.atmospheric_pm1, latest_pms_frame.atmospheric_pm25,
      latest_pms_frame.atmospheric_pm10, latest_pms_frame.cf1_pm1,
      latest_pms_frame.cf1_pm25, latest_pms_frame.cf1_pm10,
      latest_pms_frame.particle_counts[0], latest_pms_frame.particle_counts[1],
      latest_pms_frame.particle_counts[2], latest_pms_frame.particle_counts[3],
      latest_pms_frame.particle_counts[4], latest_pms_frame.particle_counts[5]);
  return latest_pms_frame.sensor_error == 0;
}

void poll_pms() {
  while (pms_serial.available() > 0) {
    const int value = pms_serial.read();
    if (value < 0 ||
        !pms_parser.push(static_cast<std::uint8_t>(value), latest_pms_frame)) {
      continue;
    }
    has_pms_frame = true;
    latest_pms_ms = millis();
    ++pms_frame_count;
  }
}

void print_periodic_pms() {
  if (!has_pms_frame) {
    Serial.printf(
        "MEAS pmsa003 values_valid=false interval_ms=%lu checksum_failures=%lu "
        "length_failures=%lu\n",
        static_cast<unsigned long>(kDisplayIntervalMs),
        static_cast<unsigned long>(pms_parser.checksum_failures()),
        static_cast<unsigned long>(pms_parser.length_failures()));
    return;
  }

  Serial.printf(
      "MEAS pmsa003 values_valid=%s sensor_error=%u frame_age_ms=%lu "
      "atmospheric_pm1_ug_m3=%u atmospheric_pm25_ug_m3=%u "
      "atmospheric_pm10_ug_m3=%u cf1_pm1_ug_m3=%u cf1_pm25_ug_m3=%u "
      "cf1_pm10_ug_m3=%u counts_per_0_1l=%u,%u,%u,%u,%u,%u frames=%lu "
      "checksum_failures=%lu length_failures=%lu\n",
      latest_pms_frame.sensor_error == 0 ? "true" : "false",
      latest_pms_frame.sensor_error,
      static_cast<unsigned long>(millis() - latest_pms_ms),
      latest_pms_frame.atmospheric_pm1, latest_pms_frame.atmospheric_pm25,
      latest_pms_frame.atmospheric_pm10, latest_pms_frame.cf1_pm1,
      latest_pms_frame.cf1_pm25, latest_pms_frame.cf1_pm10,
      latest_pms_frame.particle_counts[0], latest_pms_frame.particle_counts[1],
      latest_pms_frame.particle_counts[2], latest_pms_frame.particle_counts[3],
      latest_pms_frame.particle_counts[4], latest_pms_frame.particle_counts[5],
      static_cast<unsigned long>(pms_frame_count),
      static_cast<unsigned long>(pms_parser.checksum_failures()),
      static_cast<unsigned long>(pms_parser.length_failures()));
}

void draw_pm_triplet(std::uint16_t pm1, std::uint16_t pm25, std::uint16_t pm10,
                     int label_y, int value_y) {
  constexpr int x_positions[] = {8, 112, 216};
  constexpr const char *labels[] = {"PM1.0", "PM2.5", "PM10"};
  const std::uint16_t values[] = {pm1, pm25, pm10};

  M5.Display.setTextSize(1);
  for (std::size_t index = 0; index < 3; ++index) {
    M5.Display.setCursor(x_positions[index], label_y);
    M5.Display.print(labels[index]);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(x_positions[index], value_y);
    M5.Display.printf("%u", values[index]);
    M5.Display.setTextSize(1);
  }
}

void draw_screen_header(const char *title) {
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 5);
  M5.Display.print(title);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(279, 10);
  M5.Display.printf("%u/%u", static_cast<unsigned>(screen_page + 1),
                    static_cast<unsigned>(kScreenPageCount));

  constexpr int bar_top = 31;
  constexpr int bar_height = 184;
  const int marker_height = bar_height / kScreenPageCount;
  M5.Display.fillRect(316, bar_top, 3, bar_height, TFT_DARKGREY);
  M5.Display.fillRect(316, bar_top + marker_height * screen_page, 3,
                      marker_height, TFT_WHITE);
}

void draw_screen_footer() {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 228);
  M5.Display.print("Swipe up/down or tap for pages");
}

void draw_waiting_for_pms() {
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 48);
  M5.Display.print("Waiting for a valid frame");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 78);
  M5.Display.printf("CRC failures: %lu  length failures: %lu",
                    static_cast<unsigned long>(pms_parser.checksum_failures()),
                    static_cast<unsigned long>(pms_parser.length_failures()));
}

void draw_mass_page() {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 31);
  M5.Display.print("Mass concentration in ug/m3");
  M5.Display.setCursor(8, 48);
  M5.Display.print("Atmospheric");
  draw_pm_triplet(latest_pms_frame.atmospheric_pm1,
                  latest_pms_frame.atmospheric_pm25,
                  latest_pms_frame.atmospheric_pm10, 61, 73);

  M5.Display.setCursor(8, 104);
  M5.Display.print("CF=1");
  draw_pm_triplet(latest_pms_frame.cf1_pm1, latest_pms_frame.cf1_pm25,
                  latest_pms_frame.cf1_pm10, 117, 129);

  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 171);
  M5.Display.printf("Status: %s  sensor error=%u",
                    latest_pms_frame.sensor_error == 0 ? "valid" : "sensor",
                    latest_pms_frame.sensor_error);
  M5.Display.setCursor(8, 187);
  M5.Display.printf("Latest frame: %lu ms ago  sensor FW: %u",
                    static_cast<unsigned long>(millis() - latest_pms_ms),
                    latest_pms_frame.firmware_version);
  M5.Display.setCursor(8, 203);
  M5.Display.printf("Display and serial refresh every %lu s",
                    static_cast<unsigned long>(kDisplayIntervalMs / 1000));
}

void draw_count_pair(const char *left_label, std::uint16_t left_value,
                     const char *right_label, std::uint16_t right_value,
                     int label_y, int value_y) {
  constexpr int right_x = 168;
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, label_y);
  M5.Display.print(left_label);
  M5.Display.setCursor(right_x, label_y);
  M5.Display.print(right_label);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, value_y);
  M5.Display.printf("%u", left_value);
  M5.Display.setCursor(right_x, value_y);
  M5.Display.printf("%u", right_value);
}

void draw_counts_page() {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 31);
  M5.Display.print("Cumulative particle counts per 0.1 L");
  draw_count_pair(">0.3 um", latest_pms_frame.particle_counts[0], ">0.5 um",
                  latest_pms_frame.particle_counts[1], 49, 61);
  draw_count_pair(">1.0 um", latest_pms_frame.particle_counts[2], ">2.5 um",
                  latest_pms_frame.particle_counts[3], 91, 103);
  draw_count_pair(">5.0 um", latest_pms_frame.particle_counts[4], ">10 um",
                  latest_pms_frame.particle_counts[5], 133, 145);

  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 179);
  M5.Display.printf("Frames=%lu  CRC=%lu  length=%lu",
                    static_cast<unsigned long>(pms_frame_count),
                    static_cast<unsigned long>(pms_parser.checksum_failures()),
                    static_cast<unsigned long>(pms_parser.length_failures()));
  M5.Display.setCursor(8, 196);
  M5.Display.printf("Sensor FW=%u  error=%u  age=%lu ms",
                    latest_pms_frame.firmware_version,
                    latest_pms_frame.sensor_error,
                    static_cast<unsigned long>(millis() - latest_pms_ms));
}

void draw_device_page() {
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 35);
  M5.Display.printf("SD card: %s", sd_mounted ? "mounted" : "not mounted");
  M5.Display.setCursor(8, 55);
  M5.Display.printf("VBUS: %d mV", static_cast<int>(M5.Power.getVBUSVoltage()));
  M5.Display.setCursor(168, 55);
  M5.Display.printf("Battery: %d mV",
                    static_cast<int>(M5.Power.getBatteryVoltage()));
  M5.Display.setCursor(8, 75);
  M5.Display.printf("Battery: %ld%%  %s",
                    static_cast<long>(M5.Power.getBatteryLevel()),
                    charging_name(M5.Power.isCharging()));

  M5.Display.setCursor(8, 103);
  M5.Display.printf("Internal heap free: %lu bytes",
                    static_cast<unsigned long>(ESP.getFreeHeap()));
  M5.Display.setCursor(8, 123);
  M5.Display.printf("Internal heap minimum: %lu bytes",
                    static_cast<unsigned long>(ESP.getMinFreeHeap()));
  M5.Display.setCursor(8, 143);
  M5.Display.printf("PSRAM free: %lu / %lu bytes",
                    static_cast<unsigned long>(ESP.getFreePsram()),
                    static_cast<unsigned long>(ESP.getPsramSize()));

  M5.Display.setCursor(8, 171);
  M5.Display.printf("Uptime: %lu s",
                    static_cast<unsigned long>(millis() / 1000));
  M5.Display.setCursor(168, 171);
  M5.Display.printf("PMS frames: %lu",
                    static_cast<unsigned long>(pms_frame_count));
  M5.Display.setCursor(8, 191);
  M5.Display.printf("Touch: %s  refresh: %lu s",
                    M5.Touch.isEnabled() ? "enabled" : "disabled",
                    static_cast<unsigned long>(kDisplayIntervalMs / 1000));
}

void show_pms_screen() {
  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  constexpr const char *titles[kScreenPageCount] = {
      "PM mass", "Particle counts", "Device health"};
  draw_screen_header(titles[screen_page]);

  if (!has_pms_frame && screen_page != 2) {
    draw_waiting_for_pms();
  } else if (screen_page == 0) {
    draw_mass_page();
  } else if (screen_page == 1) {
    draw_counts_page();
  } else {
    draw_device_page();
  }
  draw_screen_footer();
}

void handle_touch_navigation() {
  if (!M5.Touch.isEnabled() || M5.Touch.getCount() == 0) {
    return;
  }

  const auto &touch = M5.Touch.getDetail();
  std::uint8_t next_page = screen_page;
  const char *gesture = nullptr;

  if (touch.wasClicked()) {
    next_page = static_cast<std::uint8_t>((screen_page + 1) % kScreenPageCount);
    gesture = "tap";
  } else if (touch.wasFlicked()) {
    const int distance_x = touch.distanceX();
    const int distance_y = touch.distanceY();
    const int absolute_x = distance_x < 0 ? -distance_x : distance_x;
    const int absolute_y = distance_y < 0 ? -distance_y : distance_y;

    if (absolute_y >= absolute_x && absolute_y >= kMinimumSwipeDistance &&
        distance_y < 0) {
      next_page =
          static_cast<std::uint8_t>((screen_page + 1) % kScreenPageCount);
      gesture = "forward-swipe";
    } else if (absolute_y >= absolute_x &&
               absolute_y >= kMinimumSwipeDistance && distance_y > 0) {
      next_page = static_cast<std::uint8_t>(
          (screen_page + kScreenPageCount - 1) % kScreenPageCount);
      gesture = "back-swipe";
    }
  }

  if (gesture == nullptr || next_page == screen_page) {
    return;
  }

  screen_page = next_page;
  show_pms_screen();
  Serial.printf("UI page=%u/%u gesture=%s\n",
                static_cast<unsigned>(screen_page + 1),
                static_cast<unsigned>(kScreenPageCount), gesture);
}

} // namespace

void setup() {
  auto config = M5.config();
  config.serial_baudrate = kSerialBaud;
  config.clear_display = true;
  config.output_power = true;
  config.internal_imu = true;
  config.internal_rtc = true;
  config.internal_mic = false;
  config.internal_spk = false;
  config.external_imu = false;
  config.external_rtc = false;
  config.external_display_value = 0;
  config.fallback_board = m5::board_t::board_M5StackCoreS3;
  M5.begin(config);
  delay(500);

  Serial.println("DIAG BEGIN schema=cores3-bringup-v1");
  report_chip();
  report_memory();
  report_i2c();
  report_power();
  report_rtc_imu_touch();
  sd_mounted = report_sd();
  start_pms();
  show_pms_screen();
  last_display_ms = millis();
  Serial.println("DIAG COMPLETE schema=cores3-bringup-v1");
}

void loop() {
  M5.update();
  poll_pms();
  handle_touch_navigation();
  const std::uint32_t now = millis();
  if (static_cast<std::uint32_t>(now - last_display_ms) >= kDisplayIntervalMs) {
    show_pms_screen();
    print_periodic_pms();
    last_display_ms = now;
  }
  delay(20);
}
