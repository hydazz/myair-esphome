#include "myair_sensor.h"

#include <cmath>

namespace esphome::myair {

void MyAirSensor::refresh() {
  const auto &session = parent_->session();
  const auto &controller = parent_->controller();
  float value = NAN;
  switch (type_) {
    case SensorType::TEMPERATURE:
      value = controller.temperature(zone_);
      break;
    case SensorType::DAMPER:
      value = controller.damper(zone_);
      break;
    case SensorType::SENSOR_REPORT:
      if (const auto counter = session.sensor_report(zone_); session.ready() && counter) value = *counter;
      break;
    case SensorType::SENSOR_ERROR:
      if (const auto flags = session.sensor_error(zone_)) value = *flags;
      break;
    case SensorType::WRITE_FAILURES:
      value = session.write_failures();
      break;
  }
  if (!has_state() || (state != value && !(std::isnan(state) && std::isnan(value)))) publish_state(value);
}

}  // namespace esphome::myair
