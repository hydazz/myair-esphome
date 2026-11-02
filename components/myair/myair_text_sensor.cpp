#include "myair_text_sensor.h"

#include <cstdio>
#include <cstring>

namespace esphome::myair {

void MyAirTextSensor::publish_(const char *value) {
  if (!has_state() || std::strcmp(get_state().c_str(), value) != 0) publish_state(value);
}

void MyAirTextSensor::refresh() {
  char text[16];
  if (type_ == TextSensorType::SENSOR_ID) {
    const auto uid = parent_->session().sensor_uid(zone_->number);
    if (uid) std::snprintf(text, sizeof(text), "%06x", static_cast<unsigned>(*uid));
    publish_(!uid ? "Unknown" : *uid == 0 ? "None" : text);
    return;
  }
  if (!parent_->session().ready()) return;
  const auto &controller = parent_->controller();
  static const char *const MODES[] = {"Off", "Auto", "Cool", "Heat", "Fan", "Dry"};
  static const char *const FANS[] = {"Auto", "Low", "Medium", "High"};
  const auto mode = controller.ac_mode();
  if (type_ == TextSensorType::AC_MODE) {
    publish_(mode ? MODES[static_cast<uint8_t>(*mode)] : "Unknown");
  } else if (type_ == TextSensorType::AC_FAN) {
    const auto fan = controller.ac_fan();
    publish_(mode == Mode::OFF ? "Off" : fan ? FANS[static_cast<uint8_t>(*fan)] : "Unknown");
  } else if (const char *name = parent_->zone_name(controller.reference())) {
    publish_(name);
  } else {
    std::snprintf(text, sizeof(text), controller.reference() == 0 ? "None" : "Zone %u", controller.reference());
    publish_(text);
  }
}

}  // namespace esphome::myair
