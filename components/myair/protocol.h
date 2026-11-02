#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esphome::myair {

// Record kinds on the bus.
namespace kind {
// Zone count and constant zones.
constexpr uint8_t CONFIG = 0x01;
// The AC's type from the control box; written to the RF module to dump its devices.
constexpr uint8_t RF_DUMP = 0x02;
// A zone's open bit, damper, sensor type, setpoint and temperature.
constexpr uint8_t ZONE = 0x03;
// A zone's damper limits and motion settings.
constexpr uint8_t LIMITS = 0x04;
// Power, mode, fan, the AC's setpoint and the reference zone.
constexpr uint8_t SYSTEM = 0x05;
// From the ESP, a request to resend every record; from the control box, its firmware, which the ESP acknowledges.
constexpr uint8_t FLUSH = 0x06;
constexpr uint8_t FIRMWARE_ACK = 0x07;
constexpr uint8_t ANNOUNCE = 0x0A;
// A sensor assignment to the control box, or a sensor's pair button from it.
constexpr uint8_t SENSOR = 0x12;
// The control box asking for its sensor assignments.
constexpr uint8_t SENSOR_TABLE = 0x13;
constexpr uint8_t RF_PAIRING = 0x26;
}  // namespace kind

// Fields of the config, zone, limits and system records.
namespace field {
constexpr uint8_t ZONE_COUNT = 1, CONSTANT_COUNT = 2, FIRST_CONSTANT = 3;
// Every zone and limits record starts with its zone number.
constexpr uint8_t ZONE_NUMBER = 0;
// The damper field holds the open bit and the position in percent.
constexpr uint8_t DAMPER = 1, SENSOR_TYPE = 2, SETPOINT = 3, WHOLE_DEGREES = 4, TENTHS = 5;
constexpr uint8_t OPEN = 0x80, POSITION = 0x7F;
constexpr uint8_t MIN_DAMPER = 1, MAX_DAMPER = 2;
constexpr uint8_t POWER = 0, MODE = 1, FAN = 2, AC_SETPOINT = 3, REFERENCE = 4;
}  // namespace field

struct Record {
  uint8_t unit_type{};
  uint8_t direction{};
  uint32_t unit_id{};
  uint8_t kind{};
  std::array<uint8_t, 7> data{};
};

struct Change {
  uint8_t kind{};
  uint8_t zone{};
  std::array<uint8_t, 7> values{};
  std::array<uint8_t, 7> mask{};

  void set(uint8_t field, uint8_t value, uint8_t bits = 0xFF);
  void merge(const Change &other);
  bool matches(const Record &record) const;
  Record apply(const Record &record) const;
};

Record encode_write(const Record &observed, const Change &change);

uint8_t crc8(std::string_view payload);
std::string frame(std::string_view payload);
std::optional<Record> parse_record(std::string_view text);
std::string encode_record(const Record &record);
std::string set_can(const std::vector<Record> &records);
bool parse_get_can(std::string_view payload, bool &accepted, std::vector<Record> &records);
Record request(uint8_t unit_type, uint32_t unit_id, uint8_t kind);

struct Frame {
  std::string payload;
  bool valid{};
};

class FrameParser {
 public:
  std::optional<Frame> feed(uint8_t byte);
  void reset() { buffer_.clear(); }

 private:
  std::string buffer_;
};

}  // namespace esphome::myair
