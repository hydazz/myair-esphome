#pragma once

#include "esphome/components/text_sensor/text_sensor.h"
#include "myair.h"

namespace esphome::myair {

enum class TextSensorType { REFERENCE_ZONE, AC_MODE, AC_FAN, AC_ERROR, SENSOR_ID };

class MyAirTextSensor : public text_sensor::TextSensor {
 public:
  MyAirTextSensor(MyAir *parent, TextSensorType type) : parent_(parent), type_(type) {
    parent_->add_on_state_callback([this]() { refresh(); });
  }
  MyAirTextSensor(Zone *zone, TextSensorType type) : MyAirTextSensor(zone->parent, type) { zone_ = zone; }
  void refresh();

 protected:
  void publish_(const char *value);
  MyAir *parent_;
  Zone *zone_{};
  TextSensorType type_;
};

}  // namespace esphome::myair
