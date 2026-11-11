#pragma once

#include "state.h"

namespace esphome::myair {

enum class Feedback : uint8_t { NOT_CONFIGURED, WAITING, POWER, UNAVAILABLE, STALE };

struct PowerConfig {
  float idle_below;
  uint32_t settle_time{5000};
  uint32_t timeout{30000};
};

class Activity {
 public:
  void set_power_config(PowerConfig config);
  void update_power(float watts, uint32_t now);
  bool tick(uint32_t now);
  Feedback feedback() const { return feedback_; }
  std::optional<Action> system(std::optional<EquipmentState> equipment) const;
  static std::optional<Action> zone(std::optional<Action> system, bool requested_on, float damper);

 private:
  std::optional<PowerConfig> config_;
  Feedback feedback_{Feedback::NOT_CONFIGURED};
  std::optional<bool> idle_;
  std::optional<bool> candidate_;
  uint32_t candidate_since_{};
  uint32_t last_sample_{};
};

}  // namespace esphome::myair
