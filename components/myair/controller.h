#pragma once

#include <array>

#include "session.h"

namespace esphome::myair {

// The range of every setpoint the ESP sends (°C).
static constexpr float MIN_SETPOINT = 16;
static constexpr float MAX_SETPOINT = 32;

// Holds in milliseconds. A simulator running house time faster shortens them.
struct Tuning {
  uint32_t reference_hold{600000};
  uint32_t trim_hold{600000};
  uint32_t changeover_hold{900000};
  uint32_t fan_hold{600000};
};

// Drives the control box toward the requested mode, fan, rooms and airflow zones. Zones with neither are left alone.
class Controller {
 public:
  explicit Controller(Session &session) : session_(session) {}
  Tuning tuning;
  void step(uint32_t now);
  void set_mode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  void set_fan(Fan fan) { fan_ = fan; }
  Fan fan() const { return fan_; }
  bool set_room(uint8_t zone, Room room);
  std::optional<Room> room(uint8_t zone) const;
  bool set_airflow(uint8_t zone, Airflow airflow);
  std::optional<Airflow> airflow(uint8_t zone) const;
  // The AC's own setpoint, which it regulates on while no temperature zone is on and the reference is 0.
  bool set_ac_setpoint(float setpoint);
  float ac_setpoint() const { return ac_setpoint_; }
  // A single target in Heat, Cool or Dry; Auto uses a range when temperature rooms are configured.
  bool set_target(float target);
  // Applies the supplied edges to every configured room, including rooms that are off. Room edits remain independent.
  bool set_targets(std::optional<float> low, std::optional<float> high);
  // The reference room, or the first configured room before a reference is selected.
  std::optional<Room> target_room() const;
  // What the AC works to: the reference room's edge, or its own setpoint while no temperature zone is the reference.
  float target() const;
  // The zone's airflow with its damper fully open, relative to the other zones'; the control box's maximum damper
  // scales it. Every zone is 1 until set.
  bool set_airflow_weight(uint8_t zone, float weight);
  float airflow_weight(uint8_t zone) const { return zone <= 10 ? airflow_weights_[zone] : 1; }
  float temperature(uint8_t zone) const;
  // The zone's setpoint in the control box, in the room's calibrated units.
  float setpoint(uint8_t zone) const;
  // The position the control box's loop sets for an open zone, in percent; 0 for a closed zone and NaN while unknown.
  float damper(uint8_t zone) const;
  bool set_offset(uint8_t zone, float offset);
  float offset(uint8_t zone) const { return zone <= 10 ? offsets_[zone] : 0; }
  bool set_damper_limit(uint8_t zone, bool maximum, float value);
  float damper_limit(uint8_t zone, bool maximum) const;

 private:
  enum class Side : uint8_t { COOL, HEAT };
  uint8_t zone_count_() const;
  bool sensor_zone_(uint8_t zone) const;
  bool on_(uint8_t zone) const { return zone <= 10 && rooms_[zone] && rooms_[zone]->on; }
  bool airflow_on_(uint8_t zone) const { return zone <= 10 && airflows_[zone] && airflows_[zone]->on; }
  bool constant_(uint8_t zone) const;
  uint16_t shortfall_() const;
  float error_(uint8_t zone, Side side) const;
  float weight_(uint8_t zone) const;
  bool carries_(uint8_t zone) const;
  // An on room with a readable temperature that carries the AC: it can run the AC and choose Auto's side, and once open
  // it is a candidate for the reference.
  bool carrying_(uint8_t zone) const;
  bool candidate_(uint8_t zone) const;
  Side choose_side_(uint32_t now);
  void trim_(uint32_t now, Side side, uint8_t reference);
  uint8_t choose_reference_(uint32_t now, Side side) const;
  uint8_t choose_fan_(uint32_t now, Side side) const;
  void observe_(uint32_t now);

  Session &session_;
  Mode mode_{Mode::OFF};
  Fan fan_{Fan::AUTO};
  std::array<std::optional<Room>, 11> rooms_{};
  std::array<std::optional<Airflow>, 11> airflows_{};
  float ac_setpoint_{24};
  std::array<float, 11> offsets_{};
  std::array<float, 11> airflow_weights_{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  std::optional<Side> auto_side_;
  uint32_t last_call_{};
  // How far each open zone's setpoint in the control box sits past its room's edge, toward more conditioning (°C).
  std::array<float, 11> trims_{};
  std::array<uint32_t, 11> trim_changed_{};
  Side trim_side_{Side::COOL};
  uint32_t generation_{};
  bool observed_{};
  uint8_t observed_fan_{};
  uint8_t observed_reference_{};
  uint32_t fan_changed_{};
  uint32_t reference_changed_{};
};

}  // namespace esphome::myair
