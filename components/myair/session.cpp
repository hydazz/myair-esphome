#include "session.h"

#include <algorithm>

namespace esphome::myair {

static uint8_t zone_key(const Record &record) {
  return record.kind == kind::ZONE || record.kind == kind::LIMITS ? record.data[field::ZONE_NUMBER] : 0;
}

bool RegisterBank::apply(const Record &record) {
  if (record.direction != 0x03 && record.direction != 0x04) return false;
  if (record.unit_id == 0 || (record.unit_type != 0x07 && record.unit_type != 0x08)) return false;
  if (!has_control_box()) {
    if (record.kind != kind::ANNOUNCE && record.kind != kind::CONFIG && record.kind != kind::SYSTEM) return false;
    unit_type_ = record.unit_type;
    unit_id_ = record.unit_id;
  }
  if (record.unit_type != unit_type_ || record.unit_id != unit_id_) return false;
  for (auto &stored : records_) {
    if (stored.kind == record.kind && zone_key(stored) == zone_key(record)) {
      if (stored.data == record.data) return false;
      stored = record;
      return true;
    }
  }
  records_.push_back(record);
  return true;
}

const Record *RegisterBank::find(uint8_t kind, uint8_t zone) const {
  for (const auto &record : records_) {
    if (record.kind == kind && zone_key(record) == zone) return &record;
  }
  return nullptr;
}

bool RegisterBank::complete() const {
  const auto *config = find(kind::CONFIG);
  if (config == nullptr || find(kind::SYSTEM) == nullptr) return false;
  for (unsigned zone = 1; zone <= config->data[field::ZONE_COUNT]; zone++) {
    if (find(kind::ZONE, zone) == nullptr || find(kind::LIMITS, zone) == nullptr) return false;
  }
  return true;
}

void Session::reset_(uint32_t now) {
  if (writes_pending()) {
    write_failures_++;
    discarded_writes_.clear();
    const auto append = [&](const Change &change, const char *reason) {
      if (!discarded_writes_.empty()) discarded_writes_ += ", ";
      discarded_writes_ += reason;
      if (change.kind == kind::SYSTEM) {
        discarded_writes_ += " AC settings";
      } else {
        discarded_writes_ += change.kind == kind::LIMITS ? " limits for zone " : " zone ";
        discarded_writes_ += std::to_string(change.zone);
      }
      const auto *observed = bank_.find(change.kind, change.zone);
      if (observed != nullptr) discarded_writes_ += " " + encode_record(encode_write(*observed, change));
    };
    for (const auto &change : active_) {
      if (std::none_of(readback_.begin(), readback_.end(), [&](const Record &r) { return change.matches(r); }))
        append(change, "unconfirmed");
    }
    for (const auto &change : writes_) append(change, "unsent");
  }
  active_.clear();
  readback_.clear();
  writes_.clear();
  sensor_index_ = 0;
  sensors_requested_ = false;
  pairing_zone_ = 0;
  generation_++;
  parser_.reset();
  bank_ = {};
  phase_ = Phase::NEGOTIATE;
  firmware_acks_.clear();
  pending_.clear();
  ack_ = -1;
  attempts_ = 0;
  retry_ = false;
  retry_missing_ = false;
  busy_ = false;
  snapshot_requested_ = false;
  ready_ = false;
  started_ = false;
  last_frame_ = last_response_ = snapshot_started_ = last_flush_ = now;
  revision_++;
}

void Session::tick(uint32_t now) {
  if (pairing_zone_ != 0 && now - pairing_started_ >= 60000) cancel_pairing(pairing_zone_);
  if (!started_) return;
  if (now - last_frame_ >= 5000 || now - last_response_ >= 10000 || (!ready_ && now - snapshot_started_ >= 10000) ||
      (!active_.empty() && now - write_started_ >= 5000))
    reset_(now);
}

std::optional<uint32_t> Session::sensor_uid(uint8_t zone) const {
  for (const auto &sensor : sensors_) {
    if (sensor.zone == zone) return sensor.uid;
  }
  return {};
}

void Session::set_sensor(uint8_t zone, uint32_t uid) {
  if (zone == 0 || uid > 0xFFFFFF || sensor_uid(zone) == uid) return;
  bool found = false;
  for (auto &sensor : sensors_) {
    if (sensor.zone == zone) {
      sensor.uid = uid;
      found = true;
    } else if (uid != 0 && sensor.uid == uid) {
      sensor.uid = 0;
    }
  }
  if (!found) sensors_.push_back({zone, uid});
  // Clear old assignments before assigning a probe to another zone.
  std::stable_partition(sensors_.begin(), sensors_.end(), [](const SensorBinding &sensor) { return sensor.uid == 0; });
  sensor_index_ = 0;
  sensors_requested_ = true;
  revision_++;
}

bool Session::start_pairing(uint8_t zone, uint32_t now) {
  const auto *config = bank_.find(kind::CONFIG);
  if (!ready_ || zone == 0 || config == nullptr || zone > config->data[field::ZONE_COUNT]) return false;
  pairing_zone_ = zone;
  pairing_started_ = now;
  revision_++;
  return true;
}

void Session::cancel_pairing(uint8_t zone) {
  if (pairing_zone_ == 0 || pairing_zone_ != zone) return;
  pairing_zone_ = 0;
  revision_++;
}

std::optional<Record> Session::desired(uint8_t kind, uint8_t zone) const {
  const auto *observed = bank_.find(kind, zone);
  if (observed == nullptr) return {};
  Record result = *observed;
  for (const auto &change : active_) {
    if (change.kind == kind && change.zone == zone) result = change.apply(result);
  }
  for (const auto &change : writes_) {
    if (change.kind == kind && change.zone == zone) result = change.apply(result);
  }
  return result;
}

bool Session::queue(const Change &change) {
  if (!ready_ || (change.kind != kind::ZONE && change.kind != kind::LIMITS && change.kind != kind::SYSTEM))
    return false;
  const auto state = desired(change.kind, change.zone);
  if (!state) return false;
  if (change.matches(*state)) return true;
  for (auto &queued : writes_) {
    if (queued.kind == change.kind && queued.zone == change.zone) {
      queued.merge(change);
      revision_++;
      return true;
    }
  }
  writes_.push_back(change);
  revision_++;
  return true;
}

std::string Session::receive(uint8_t byte, uint32_t now) {
  tick(now);
  auto message = parser_.feed(byte);
  if (!message) return {};
  if (message->valid) {
    last_frame_ = now;
    if (!started_) {
      started_ = true;
      last_response_ = snapshot_started_ = now;
    }
    if (message->payload == "Ping") return on_ping_(now);
  }
  handle_(*message, now);
  return {};
}

void Session::handle_(const Frame &message, uint32_t now) {
  if (message.valid && message.payload.find("CAN2 in use") != std::string::npos) {
    busy_ = true;
    return;
  }
  if (message.payload.compare(0, 6, "getCAN") != 0) return;
  bool accepted;
  std::vector<Record> records;
  if (!message.valid || !parse_get_can(message.payload, accepted, records)) {
    ack_ = 0;
    return;
  }
  ack_ = 1;
  busy_ = false;
  last_response_ = now;
  if (!accepted) {
    retry_ = !pending_.empty();
    return;
  }
  pending_.clear();
  attempts_ = 0;
  retry_ = false;

  // Find the control box before applying records; announcements need not come first.
  bool changed = false;
  const bool had_control_box = bank_.has_control_box();
  for (const auto &record : records) {
    if (record.kind == kind::ANNOUNCE || record.kind == kind::CONFIG) changed |= bank_.apply(record);
  }
  for (const auto &record : records) {
    // A command can change the mode and several zone targets. Keep their readback together so consumers never
    // combine one side's mode with the other side's targets, even when the echoes arrive in separate frames.
    const bool readback =
        from_control_box_(record) && std::any_of(active_.begin(), active_.end(), [&](const Change &change) {
          return change.kind == record.kind && change.zone == zone_key(record);
        });
    if (readback) {
      const auto previous = std::find_if(readback_.begin(), readback_.end(), [&](const Record &item) {
        return item.kind == record.kind && zone_key(item) == zone_key(record);
      });
      if (previous == readback_.end())
        readback_.push_back(record);
      else
        *previous = record;
      continue;
    }
    changed |= bank_.apply(record);
    // Bit 6 of a sensor record's info byte is its pair button.
    if (pairing_zone_ != 0 && record.kind == kind::SENSOR && from_control_box_(record) &&
        (record.data[3] & 0x40) != 0) {
      const uint32_t uid = (uint32_t{record.data[0]} << 16) | (uint32_t{record.data[1]} << 8) | record.data[2];
      if (uid != 0) {
        set_sensor(pairing_zone_, uid);
        cancel_pairing(pairing_zone_);
      }
    }
    if (pairing_zone_ != 0 && pairing_active_zone_ == pairing_zone_ && record.kind == kind::RF_PAIRING &&
        (record.unit_type == 7 || record.unit_type == 8) && record.unit_id == bank_.unit_id() &&
        (record.direction == 3 || record.direction == 4) && (record.data[1] == 4 || record.data[1] == 0x81) &&
        record.data[2] == pairing_zone_) {
      const uint32_t uid = (uint32_t{record.data[3]} << 16) | (uint32_t{record.data[4]} << 8) | record.data[5];
      const uint32_t sensor = record.data[1] == 4 ? uid & 0xFFFFF : uid;
      if (record.data[0] == 2 && sensor != 0 && uid != 0xFFFFFF) {
        set_sensor(pairing_zone_, sensor);
        pairing_active_zone_ = 0;
        cancel_pairing(pairing_zone_);
      } else if (record.data[0] == 3) {
        pairing_active_zone_ = 0;
        cancel_pairing(pairing_zone_);
      }
    }
    if (record.kind == kind::SENSOR_TABLE && from_control_box_(record)) {
      sensor_index_ = 0;
      sensors_requested_ = true;
    }
    if (record.kind == kind::FLUSH && record.unit_id != 0 && (record.direction == 0x03 || record.direction == 0x04) &&
        (record.unit_type == 0x07 || record.unit_type == 0x08)) {
      const auto reply = request(record.unit_type, record.unit_id, kind::FIRMWARE_ACK);
      const auto duplicate = std::find_if(firmware_acks_.begin(), firmware_acks_.end(), [&](const Record &item) {
        return item.unit_type == reply.unit_type && item.unit_id == reply.unit_id;
      });
      if (duplicate == firmware_acks_.end()) firmware_acks_.push_back(reply);
    }
  }
  if (!active_.empty() && std::all_of(active_.begin(), active_.end(), [&](const Change &change) {
        return std::any_of(readback_.begin(), readback_.end(),
                           [&](const Record &record) { return change.matches(record); });
      })) {
    for (const auto &record : readback_) bank_.apply(record);
    active_.clear();
    readback_.clear();
    changed = true;
  }
  if (!had_control_box && bank_.has_control_box()) {
    sensor_index_ = 0;
    sensors_requested_ = true;
  }
  const bool ready = snapshot_requested_ && bank_.complete();
  changed |= ready != ready_;
  ready_ = ready;
  if (changed) revision_++;
}

bool Session::from_control_box_(const Record &record) const {
  return bank_.has_control_box() && record.unit_type == bank_.unit_type() && record.unit_id == bank_.unit_id() &&
         (record.direction == 3 || record.direction == 4);
}

std::string Session::send_(std::string payload, bool retry_missing) {
  pending_ = std::move(payload);
  attempts_ = 1;
  retry_ = false;
  retry_missing_ = retry_missing;
  return frame(pending_);
}

std::string Session::on_ping_(uint32_t now) {
  if (ack_ >= 0) {
    const auto payload = ack_ == 1 ? "ackCAN 1" : "ackCAN 0";
    ack_ = -1;
    return frame(payload);
  }
  if (!pending_.empty()) {
    if (retry_ || retry_missing_) {
      if (attempts_ < 3) {
        attempts_++;
        retry_ = false;
        return frame(pending_);
      }
      reset_(now);
      started_ = true;
    } else {
      pending_.clear();
    }
  }
  if (!firmware_acks_.empty()) {
    const auto count = std::min<size_t>(25, firmware_acks_.size());
    std::vector<Record> replies(firmware_acks_.begin(), firmware_acks_.begin() + count);
    firmware_acks_.erase(firmware_acks_.begin(), firmware_acks_.begin() + count);
    return send_(set_can(replies));
  }
  if (phase_ == Phase::NEGOTIATE) {
    phase_ = Phase::RESET;
    return frame("getSystemData");
  }
  if (phase_ == Phase::RESET) {
    phase_ = Phase::DISCOVER;
    // Some control boxes do not answer the global dirty reset.
    return send_(set_can({request(0x07, 0, kind::FLUSH)}), false);
  }
  if (bank_.has_control_box() && !snapshot_requested_) {
    bank_.clear_records();
    snapshot_requested_ = true;
    phase_ = Phase::SNAPSHOT;
    last_flush_ = now;
    return send_(set_can({request(bank_.unit_type(), 0, kind::FLUSH)}));
  }
  if (phase_ == Phase::DISCOVER) {
    phase_ = Phase::SNAPSHOT;
    auto rf_dump = request(0x08, 0, kind::RF_DUMP);
    rf_dump.data[0] = 0x36;
    return send_(set_can({request(0x08, 0, kind::FLUSH), rf_dump}));
  }
  if (ready_ && pairing_active_zone_ != pairing_zone_) {
    auto pairing = request(0x07, bank_.unit_id(), kind::RF_PAIRING);
    pairing.data[1] = 0x81;
    if (pairing_active_zone_ != 0) {
      pairing.data[2] = pairing_active_zone_;
      pairing_active_zone_ = 0;
    } else {
      pairing.data[0] = 1;
      pairing.data[2] = pairing_zone_;
      pairing_active_zone_ = pairing_zone_;
    }
    return send_(set_can({pairing}));
  }
  const auto *config = bank_.find(kind::CONFIG);
  if (sensors_requested_ && config != nullptr) {
    std::vector<Record> bindings;
    while (sensor_index_ < sensors_.size() && bindings.size() < 25) {
      const auto &sensor = sensors_[sensor_index_++];
      if (sensor.zone > config->data[field::ZONE_COUNT]) continue;
      auto binding = request(bank_.unit_type(), bank_.unit_id(), kind::SENSOR);
      binding.data = {static_cast<uint8_t>(sensor.uid >> 16),
                      static_cast<uint8_t>(sensor.uid >> 8),
                      static_cast<uint8_t>(sensor.uid),
                      sensor.zone,
                      0,
                      0,
                      0};
      bindings.push_back(binding);
    }
    sensors_requested_ = sensor_index_ < sensors_.size();
    if (!bindings.empty()) return send_(set_can(bindings));
  }
  if (ready_ && active_.empty()) {
    std::vector<Record> records;
    while (!writes_.empty() && records.size() < 25) {
      const auto change = writes_.front();
      writes_.erase(writes_.begin());
      const auto *observed = bank_.find(change.kind, change.zone);
      revision_++;
      if (observed == nullptr || change.matches(*observed)) continue;
      active_.push_back(change);
      records.push_back(encode_write(*observed, change));
    }
    if (!records.empty()) {
      write_started_ = now;
      readback_requested_ = false;
      return send_(set_can(records));
    }
  }
  if (!active_.empty() && !readback_requested_ && now - write_started_ >= 1000) {
    readback_requested_ = true;
    return send_(set_can({request(bank_.unit_type(), 0, kind::FLUSH)}));
  }
  if (ready_ && now - last_flush_ >= 60000) {
    last_flush_ = now;
    return send_(set_can({request(bank_.unit_type(), 0, kind::FLUSH)}));
  }
  if (busy_) return {};
  return frame("setCAN ");
}

}  // namespace esphome::myair
