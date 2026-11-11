#include "activity.h"

#include <cmath>

namespace esphome::myair {

Feedback Activity::feedback() const {
  if (!idle_below_) return Feedback::NOT_CONFIGURED;
  return idle_.has_value() ? Feedback::POWER : Feedback::UNAVAILABLE;
}

void Activity::update_power(float watts) {
  if (idle_below_ && std::isfinite(watts) && watts >= 0)
    idle_ = watts < *idle_below_;
  else
    idle_.reset();
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
