#pragma once

#include "protocol.h"

namespace esphome::myair {

class RegisterBank {
 public:
  bool apply(const Record &record);
  const Record *find(uint8_t kind, uint8_t zone = 0) const;
  bool complete() const;
  bool has_control_box() const { return unit_id_ != 0; }
  uint8_t unit_type() const { return unit_type_; }
  uint32_t unit_id() const { return unit_id_; }
  void clear_records() { records_.clear(); }

 private:
  uint8_t unit_type_{};
  uint32_t unit_id_{};
  std::vector<Record> records_;
};

class Session {
 public:
  void set_sensor(uint8_t zone, uint32_t uid);
  std::optional<uint32_t> sensor_uid(uint8_t zone) const;
  bool start_pairing(uint8_t zone, uint32_t now);
  void cancel_pairing(uint8_t zone);
  uint8_t pairing_zone() const { return pairing_zone_; }
  std::string receive(uint8_t byte, uint32_t now);
  void tick(uint32_t now);
  const RegisterBank &bank() const { return bank_; }
  bool ready() const { return ready_; }
  uint32_t revision() const { return revision_; }
  uint32_t generation() const { return generation_; }
  uint32_t write_failures() const { return write_failures_; }
  const std::string &discarded_writes() const { return discarded_writes_; }
  bool queue(const Change &change);
  std::optional<Record> desired(uint8_t kind, uint8_t zone = 0) const;
  bool writes_pending() const { return !active_.empty() || !writes_.empty(); }

 private:
  struct SensorBinding {
    uint8_t zone;
    uint32_t uid;
  };
  enum class Phase { NEGOTIATE, RESET, DISCOVER, SNAPSHOT };
  void reset_(uint32_t now);
  void handle_(const Frame &message, uint32_t now);
  std::string on_ping_(uint32_t now);
  std::string send_(std::string payload, bool retry_missing = true);
  // The record comes from the control box this session talks to.
  bool from_control_box_(const Record &record) const;

  FrameParser parser_;
  RegisterBank bank_;
  Phase phase_{Phase::NEGOTIATE};
  std::vector<Record> firmware_acks_;
  std::string pending_;
  int ack_{-1};
  uint8_t attempts_{};
  bool retry_{};
  bool retry_missing_{};
  bool busy_{};
  bool snapshot_requested_{};
  bool ready_{};
  bool started_{};
  uint32_t last_frame_{};
  uint32_t last_response_{};
  uint32_t snapshot_started_{};
  uint32_t last_flush_{};
  uint32_t revision_{};
  uint32_t generation_{};
  uint32_t write_failures_{};
  std::string discarded_writes_;
  std::vector<Change> writes_;
  std::vector<Change> active_;
  std::vector<Record> readback_;
  uint32_t write_started_{};
  bool readback_requested_{};
  std::vector<SensorBinding> sensors_;
  size_t sensor_index_{};
  bool sensors_requested_{};
  uint8_t pairing_zone_{};
  uint8_t pairing_active_zone_{};
  uint32_t pairing_started_{};
};

}  // namespace esphome::myair
