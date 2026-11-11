#pragma once

#include "activity.h"
#include "controller.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome::myair {

class MyAir;

class Zone {
 public:
  Zone(MyAir *parent, uint8_t number, const char *name, bool sensor = true)
      : parent(parent), number(number), name(name), sensor(sensor) {}
  MyAir *parent;
  uint8_t number;
  const char *name;
  // Temperature zones have a sensor and a room; airflow zones have neither and hold a damper position.
  bool sensor;
  void restore();
  void save_sensor();
  void save_state();
  void set_offset(float value);

 private:
  ESPPreferenceObject sensor_preference_;
  ESPPreferenceObject offset_preference_;
  ESPPreferenceObject state_preference_;
  std::optional<uint32_t> saved_sensor_uid_;
};

class MyAir : public Component, public uart::UARTDevice {
  friend class Zone;

 public:
  MyAir() : controller_(session_) {}
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  const Controller &controller() const { return controller_; }
  // The holds, for a simulator that runs house time faster.
  Tuning &tuning() { return controller_.tuning; }
  const Session &session() const { return session_; }
  std::optional<Action> activity() const { return activity_.system(session_.equipment()); }
  Feedback feedback() const { return activity_.feedback(); }
  void set_power_feedback(sensor::Sensor *sensor, float idle_below, uint32_t settle_time, uint32_t timeout);
  void set_mode(Mode mode);
  void set_fan(Fan fan);
  bool set_room(uint8_t zone, const Room &room);
  bool set_airflow(uint8_t zone, const Airflow &airflow);
  bool set_damper_limit(uint8_t zone, bool maximum, float value);
  // Climate setters save intent; the climate calls refresh once after applying the whole request.
  bool set_target(float target);
  bool set_targets(std::optional<float> low, std::optional<float> high);
  bool has_temperature_zones() const;
  void pair_sensor(uint8_t zone, bool enabled);
  void unpair_sensor(uint8_t zone);
  const char *zone_name(uint8_t number) const;
  void add_zone(Zone *zone) { zones_.push_back(zone); }
  void set_airflow_weight(uint8_t zone, float weight) { controller_.set_airflow_weight(zone, weight); }
  void set_sensor(uint8_t zone, uint32_t uid) { session_.set_sensor(zone, uid); }
  void refresh();
  template <typename F>
  void add_on_state_callback(F &&callback) {
    state_callbacks_.add(std::forward<F>(callback));
  }

 private:
  void publish_();
  void save_system_();
  void save_targets_();
  Zone *zone_(uint8_t number) const;
  Session session_;
  Controller controller_;
  Activity activity_;
  ESPPreferenceObject system_preference_;
  ESPPreferenceObject setpoint_preference_;
  uint32_t revision_{};
  uint32_t failures_{};
  std::vector<Zone *> zones_;
  CallbackManager<void()> state_callbacks_;
};

}  // namespace esphome::myair
