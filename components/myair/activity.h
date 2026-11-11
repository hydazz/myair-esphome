#pragma once

#include "state.h"

namespace esphome::myair {

enum class Feedback : uint8_t { NOT_CONFIGURED, POWER, UNAVAILABLE };

class Activity {
 public:
  void set_idle_below(float watts) { idle_below_ = watts; }
  void update_power(float watts);
  Feedback feedback() const;
  std::optional<Action> system(std::optional<EquipmentState> equipment) const;
  static std::optional<Action> zone(std::optional<Action> system, bool requested_on, float damper);

 private:
  std::optional<float> idle_below_;
  std::optional<bool> idle_;
};

}  // namespace esphome::myair
