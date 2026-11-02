#pragma once

#include "esphome/components/sensor/sensor.h"
#include "myair.h"

namespace esphome::myair {

enum class SensorType { TEMPERATURE, DAMPER };

class ZoneSensor : public sensor::Sensor {
 public:
  ZoneSensor(Zone *zone, SensorType type) : zone_(zone), type_(type) {
    zone_->parent->add_on_state_callback([this]() { refresh(); });
  }
  void refresh();

 protected:
  Zone *zone_;
  SensorType type_;
};

}  // namespace esphome::myair
