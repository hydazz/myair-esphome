#include "myair_text_sensor.h"

#include <cstdio>
#include <cstring>

namespace esphome::myair {

void MyAirTextSensor::publish_(const char *value) {
  if (!has_state() || std::strcmp(get_state().c_str(), value) != 0) publish_state(value);
}

void MyAirTextSensor::refresh() {
  char text[16];
  if (type_ == TextSensorType::ACTIVITY_FEEDBACK) {
    static const char *const FEEDBACK[] = {"Not configured", "Waiting", "Power", "Unavailable", "Stale"};
    publish_(FEEDBACK[static_cast<uint8_t>(parent_->feedback())]);
    return;
  }
  if (type_ == TextSensorType::SENSOR_ID) {
    const auto uid = parent_->session().sensor_uid(zone_->number);
    if (uid) std::snprintf(text, sizeof(text), "%06x", static_cast<unsigned>(*uid));
    publish_(!uid ? "Unknown" : *uid == 0 ? "None" : text);
    return;
  }
  const auto &session = parent_->session();
  if (!session.ready()) {
    publish_("Unknown");
    return;
  }
  if (type_ == TextSensorType::AC_ERROR) {
    const auto *record = session.bank().find(kind::AC_ERROR);
    if (record == nullptr) {
      publish_("Unknown");
      return;
    }
    // The CB10 sends a five-character, padded error code.
    size_t begin = 0, end = 5;
    while (begin < end && record->data[begin] <= ' ') ++begin;
    while (end > begin && record->data[end - 1] <= ' ') --end;
    std::memcpy(text, record->data.data() + begin, end - begin);
    text[end - begin] = '\0';
    publish_(end == begin ? "None" : text);
    return;
  }
  const auto equipment = session.equipment();
  if (!equipment) {
    publish_("Unknown");
    return;
  }
  static const char *const MODES[] = {"Off", "Auto", "Cool", "Heat", "Fan", "Dry"};
  static const char *const FANS[] = {"Auto", "Low", "Medium", "High"};
  const auto mode = equipment->mode;
  if (type_ == TextSensorType::AC_MODE) {
    publish_(!equipment->enabled ? "Off" : mode ? MODES[static_cast<uint8_t>(*mode)] : "Unknown");
  } else if (type_ == TextSensorType::AC_FAN) {
    const auto fan = equipment->fan;
    publish_(!equipment->enabled ? "Off" : fan ? FANS[static_cast<uint8_t>(*fan)] : "Unknown");
  } else if (const char *name = parent_->zone_name(equipment->reference)) {
    publish_(name);
  } else {
    std::snprintf(text, sizeof(text), equipment->reference == 0 ? "None" : "Zone %u", equipment->reference);
    publish_(text);
  }
}

}  // namespace esphome::myair
