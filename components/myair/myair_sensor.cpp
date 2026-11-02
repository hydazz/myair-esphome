#include "myair_sensor.h"

#include <cmath>

namespace esphome::myair {

void ZoneSensor::refresh() {
  const auto &controller = zone_->parent->controller();
  const float value =
      type_ == SensorType::TEMPERATURE ? controller.temperature(zone_->number) : controller.damper(zone_->number);
  if (!has_state() || (state != value && !(std::isnan(state) && std::isnan(value)))) publish_state(value);
}

}  // namespace esphome::myair
