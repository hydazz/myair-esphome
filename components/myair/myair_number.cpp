#include "myair_number.h"

#include <cmath>

namespace esphome::myair {

void ZoneNumber::control(float value) {
  if (type_ == NumberType::TEMPERATURE_OFFSET) {
    zone_->set_offset(value);
    return;
  }
  zone_->parent->set_damper_limit(zone_->number, type_ == NumberType::MAX_DAMPER, value);
}

void ZoneNumber::refresh() {
  const auto &controller = zone_->parent->controller();
  const float value = type_ == NumberType::TEMPERATURE_OFFSET
                          ? controller.offset(zone_->number)
                          : controller.damper_limit(zone_->number, type_ == NumberType::MAX_DAMPER);
  if (!has_state() || (state != value && !(std::isnan(state) && std::isnan(value)))) publish_state(value);
}

}  // namespace esphome::myair
