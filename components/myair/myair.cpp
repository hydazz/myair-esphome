#include "myair.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::myair {

static const char *const TAG = "myair";

// Mode and fan as the controller's enum values.
struct SystemSettings {
  uint8_t mode;
  uint8_t fan;
};

void Zone::restore() {
  sensor_preference_ = global_preferences->make_preference<uint32_t>(0x6D790100 + number);
  uint32_t uid;
  if (sensor_preference_.load(&uid) && uid <= 0xFFFFFF) {
    saved_sensor_uid_ = uid;
    parent->set_sensor(number, uid);
  } else {
    // Only pairing and unpairing save, so the zone's sensor_uid holds until the zone is paired or unpaired.
    saved_sensor_uid_ = parent->session().sensor_uid(number);
  }
  offset_preference_ = global_preferences->make_preference<float>(0x6D790200 + number);
  float offset;
  if (offset_preference_.load(&offset)) parent->controller_.set_offset(number, offset);
  // A listed zone is always controlled; it starts off until it is turned on.
  auto &controller = parent->controller_;
  if (sensor) {
    state_preference_ = global_preferences->make_preference<Room>(0x6D790300 + number);
    Room room;
    if (!state_preference_.load(&room) || !controller.set_room(number, room)) controller.set_room(number, Room{});
  } else {
    state_preference_ = global_preferences->make_preference<Airflow>(0x6D790400 + number);
    Airflow airflow;
    if (!state_preference_.load(&airflow) || !controller.set_airflow(number, airflow))
      controller.set_airflow(number, Airflow{});
  }
}

void Zone::save_state() {
  const auto &controller = parent->controller_;
  if (auto room = controller.room(number)) state_preference_.save(&*room);
  if (auto airflow = controller.airflow(number)) state_preference_.save(&*airflow);
}

void Zone::save_sensor() {
  const auto uid = parent->session().sensor_uid(number);
  if (uid && uid != saved_sensor_uid_) {
    sensor_preference_.save(&*uid);
    saved_sensor_uid_ = uid;
    ESP_LOGI(TAG, "Zone %u sensor: %06x", number, static_cast<unsigned>(*uid));
  }
}

void Zone::set_offset(float value) {
  if (parent->controller_.set_offset(number, value)) offset_preference_.save(&value);
  parent->refresh();
}

void MyAir::setup() {
  system_preference_ = global_preferences->make_preference<SystemSettings>(0x6D790000);
  SystemSettings saved;
  if (system_preference_.load(&saved) && saved.mode <= static_cast<uint8_t>(Mode::DRY) &&
      saved.fan <= static_cast<uint8_t>(Fan::HIGH)) {
    controller_.set_mode(static_cast<Mode>(saved.mode));
    controller_.set_fan(static_cast<Fan>(saved.fan));
  }
  setpoint_preference_ = global_preferences->make_preference<float>(0x6D790001);
  float setpoint;
  if (setpoint_preference_.load(&setpoint)) controller_.set_ac_setpoint(setpoint);
  for (auto *zone : zones_) zone->restore();
  publish_();
}

void MyAir::set_mode(Mode mode) {
  controller_.set_mode(mode);
  save_system_();
  refresh();
}

void MyAir::set_fan(Fan fan) {
  controller_.set_fan(fan);
  save_system_();
  refresh();
}

void MyAir::save_system_() {
  const SystemSettings settings{static_cast<uint8_t>(controller_.mode()), static_cast<uint8_t>(controller_.fan())};
  system_preference_.save(&settings);
}

Zone *MyAir::zone_(uint8_t number) const {
  for (auto *zone : zones_) {
    if (zone->number == number) return zone;
  }
  return nullptr;
}

bool MyAir::set_room(uint8_t zone, const Room &room) {
  auto *found = zone_(zone);
  const bool accepted = found != nullptr && found->sensor && controller_.set_room(zone, room);
  if (accepted)
    found->save_state();
  else
    ESP_LOGW(TAG, "Zone %u: rejected range %.0f-%.0f", zone, room.heat_to, room.cool_to);
  refresh();
  return accepted;
}

bool MyAir::set_airflow(uint8_t zone, const Airflow &airflow) {
  auto *found = zone_(zone);
  const bool accepted = found != nullptr && !found->sensor && controller_.set_airflow(zone, airflow);
  if (accepted)
    found->save_state();
  else
    ESP_LOGW(TAG, "Zone %u: rejected damper position %u", zone, airflow.position);
  refresh();
  return accepted;
}

bool MyAir::set_damper_limit(uint8_t zone, bool maximum, float value) {
  const bool accepted = controller_.set_damper_limit(zone, maximum, value);
  if (!accepted) ESP_LOGW(TAG, "Zone %u: rejected %s damper %.0f", zone, maximum ? "maximum" : "minimum", value);
  refresh();
  return accepted;
}

bool MyAir::set_target(float target) {
  const bool accepted = controller_.set_target(target, millis());
  if (accepted) {
    for (auto *zone : zones_) {
      if (zone->sensor) zone->save_state();
    }
    const float saved = controller_.ac_setpoint();
    setpoint_preference_.save(&saved);
  } else {
    ESP_LOGW(TAG, "Rejected target %.1f", target);
  }
  refresh();
  return accepted;
}

void MyAir::pair_sensor(uint8_t zone, bool enabled) {
  if (enabled) {
    if (session_.start_pairing(zone, millis()))
      ESP_LOGI(TAG, "Zone %u: waiting 60 seconds for a sensor pairing button", zone);
    else
      ESP_LOGW(TAG, "Zone %u: pairing requires a connected control box", zone);
  } else {
    session_.cancel_pairing(zone);
  }
  publish_();
}

void MyAir::unpair_sensor(uint8_t zone) {
  session_.cancel_pairing(zone);
  session_.set_sensor(zone, 0);
  publish_();
}

const char *MyAir::zone_name(uint8_t number) const {
  for (const auto *zone : zones_) {
    if (zone->number == number) return zone->name;
  }
  return nullptr;
}

void MyAir::refresh() {
  controller_.step(millis());
  publish_();
}

void MyAir::loop() {
  session_.tick(millis());
  uint8_t byte;
  while (available() && read_byte(&byte)) {
    const auto reply = session_.receive(byte, millis());
    if (!reply.empty()) {
      ESP_LOGV(TAG, "TX: %s", reply.c_str());
      write_str(reply.c_str());
    }
  }
  controller_.step(millis());
  if (revision_ != session_.revision()) {
    revision_ = session_.revision();
    publish_();
  }
  if (failures_ != session_.write_failures()) {
    failures_ = session_.write_failures();
    ESP_LOGW(TAG, "Unconfirmed command discarded while resynchronising");
  }
}

void MyAir::publish_() {
  for (auto *zone : zones_) zone->save_sensor();
  if (session_.ready())
    status_clear_warning();
  else
    status_set_warning();
  state_callbacks_.call();
}

void MyAir::dump_config() {
  ESP_LOGCONFIG(TAG, "MyAir:");
  for (const auto *zone : zones_) {
    ESP_LOGCONFIG(TAG, "  Zone %u: %s, %s, airflow weight %.2g", zone->number, zone->name,
                  zone->sensor ? "temperature" : "airflow", controller_.airflow_weight(zone->number));
  }
}

}  // namespace esphome::myair
