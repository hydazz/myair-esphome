#include "activity.h"

#include <cmath>

namespace esphome::myair {

void Activity::set_power_config(PowerConfig config) {
  config_ = config;
  feedback_ = Feedback::WAITING;
  idle_.reset();
  candidate_.reset();
}

bool Activity::tick(uint32_t now) {
  if (!config_ || feedback_ == Feedback::UNAVAILABLE || feedback_ == Feedback::STALE ||
      now - last_sample_ < config_->timeout)
    return false;
  feedback_ = Feedback::STALE;
  idle_.reset();
  candidate_.reset();
  return true;
}

void Activity::update_power(float watts, uint32_t now) {
  if (!config_) return;
  tick(now);
  if (!std::isfinite(watts) || watts < 0) {
    feedback_ = Feedback::UNAVAILABLE;
    idle_.reset();
    candidate_.reset();
    return;
  }
  last_sample_ = now;
  const bool next = watts < config_->idle_below;
  if (idle_ == next) {
    candidate_.reset();
  } else if (candidate_ != next) {
    candidate_ = next;
    candidate_since_ = now;
  } else if (now - candidate_since_ >= config_->settle_time) {
    idle_ = next;
    candidate_.reset();
  }
  feedback_ = idle_.has_value() ? Feedback::POWER : Feedback::WAITING;
}

std::optional<Action> Activity::system(std::optional<EquipmentState> equipment) const {
  if (!equipment) return {};
  if (!equipment->enabled || equipment->mode == Mode::OFF) return Action::OFF;
  if (!equipment->mode) return {};
  if (idle_ == true) return Action::IDLE;
  switch (*equipment->mode) {
    case Mode::COOL:
      return Action::COOLING;
    case Mode::HEAT:
      return Action::HEATING;
    case Mode::FAN:
      return Action::FAN;
    case Mode::DRY:
      return Action::DRYING;
    default:
      return {};
  }
}

std::optional<Action> Activity::zone(std::optional<Action> system, bool requested_on, float damper) {
  if (!system || system == Action::OFF) return system;
  if (!requested_on) return Action::OFF;
  if (system == Action::IDLE) return system;
  if (!std::isfinite(damper)) return {};
  // The CB10 holds satisfied temperature zones at 5%, not closed. Fan mode has no temperature demand.
  const float idle_position = system == Action::FAN ? 0 : 5;
  return damper > idle_position ? system : Action::IDLE;
}

}  // namespace esphome::myair
