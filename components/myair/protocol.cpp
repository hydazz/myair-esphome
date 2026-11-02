#include "protocol.h"

#include <cstdio>

namespace esphome::myair {

void Change::set(uint8_t field, uint8_t value, uint8_t bits) {
  values[field] = (values[field] & ~bits) | (value & bits);
  mask[field] |= bits;
}

void Change::merge(const Change &other) {
  for (size_t i = 0; i < mask.size(); i++) set(i, other.values[i], other.mask[i]);
}

bool Change::matches(const Record &record) const {
  if (record.kind != kind || ((kind == 3 || kind == 4) && record.data[0] != zone)) return false;
  for (size_t i = 0; i < mask.size(); i++) {
    if ((record.data[i] & mask[i]) != (values[i] & mask[i])) return false;
  }
  return true;
}

Record Change::apply(const Record &record) const {
  Record result = record;
  for (size_t i = 0; i < mask.size(); i++) result.data[i] = (result.data[i] & ~mask[i]) | (values[i] & mask[i]);
  return result;
}

Record encode_write(const Record &observed, const Change &change) {
  Record result = change.apply(observed);
  result.direction = 1;
  if (result.kind == 3) {
    // Sensor type and measured temperature are report-only fields.
    result.data[2] = result.data[4] = result.data[5] = result.data[6] = 0;
  } else if (result.kind == 4) {
    // Motion status, errors and RSSI are report-only fields.
    result.data[3] = result.data[5] = result.data[6] = 0;
  } else if (result.kind == 5) {
    result.data[6] = 0;
  }
  return result;
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parse_hex(std::string_view text, uint32_t &value) {
  value = 0;
  for (char c : text) {
    const int digit = hex_digit(c);
    if (digit < 0) return false;
    value = (value << 4) | static_cast<uint32_t>(digit);
  }
  return true;
}

uint8_t crc8(std::string_view payload) {
  uint8_t crc = 0;
  for (uint8_t byte : payload) {
    crc ^= byte;
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xB2 : 0);
  }
  return crc ^ 0xFF;
}

std::string frame(std::string_view payload) {
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), "</U=%02x>", crc8(payload));
  return "<U>" + std::string(payload) + suffix;
}

std::optional<Record> parse_record(std::string_view text) {
  if (text.size() != 25) return {};
  for (char c : text) {
    if (hex_digit(c) < 0) return {};
  }
  auto field = [&](size_t offset, size_t count = 2) {
    uint32_t value;
    parse_hex(text.substr(offset, count), value);
    return value;
  };
  Record record;
  record.unit_type = field(0);
  record.direction = field(2);
  record.unit_id = field(4, 5);
  record.kind = field(9);
  for (size_t i = 0; i < record.data.size(); i++) record.data[i] = field(11 + i * 2);
  return record;
}

std::string encode_record(const Record &record) {
  char text[26];
  std::snprintf(text, sizeof(text), "%02x%02x%05x%02x", record.unit_type, record.direction,
                static_cast<unsigned>(record.unit_id), record.kind);
  for (size_t i = 0; i < record.data.size(); i++) {
    std::snprintf(text + 11 + i * 2, 3, "%02x", record.data[i]);
  }
  return text;
}

std::string set_can(const std::vector<Record> &records) {
  std::string payload = "setCAN ";
  for (const auto &record : records) {
    if (payload.size() > 7) payload += ' ';
    payload += encode_record(record);
  }
  return payload;
}

bool parse_get_can(std::string_view payload, bool &accepted, std::vector<Record> &records) {
  records.clear();
  if (payload.size() < 8 || payload.substr(0, 7) != "getCAN " || (payload[7] != '0' && payload[7] != '1')) return false;
  if (payload.size() > 8 && payload[8] != ' ') return false;
  accepted = payload[7] == '1';
  payload.remove_prefix(8);
  std::vector<Record> parsed;
  while (!payload.empty()) {
    if (payload.front() == ' ') {
      payload.remove_prefix(1);
      continue;
    }
    const auto end = payload.find(' ');
    auto record = parse_record(payload.substr(0, end));
    if (!record) return false;
    parsed.push_back(*record);
    if (end == std::string_view::npos) break;
    payload.remove_prefix(end);
  }
  records = std::move(parsed);
  return true;
}

Record request(uint8_t unit_type, uint32_t unit_id, uint8_t kind) { return {unit_type, 0x01, unit_id, kind, {}}; }

std::optional<Frame> FrameParser::feed(uint8_t byte) {
  buffer_ += static_cast<char>(byte);
  auto start = buffer_.rfind("<U>");
  if (start == std::string::npos) {
    if (buffer_.size() > 2) buffer_.erase(0, buffer_.size() - 2);
    return {};
  }
  if (start != 0) buffer_.erase(0, start);
  if (buffer_.size() > 2048) {
    reset();
    return {};
  }
  const auto end = buffer_.find("</U=", 3);
  if (end == std::string::npos || buffer_.size() < end + 7) return {};
  uint32_t checksum;
  Frame result{buffer_.substr(3, end - 3), false};
  result.valid = buffer_[end + 6] == '>' && parse_hex(std::string_view(buffer_).substr(end + 4, 2), checksum) &&
                 checksum == crc8(result.payload);
  reset();
  return result;
}

}  // namespace esphome::myair
