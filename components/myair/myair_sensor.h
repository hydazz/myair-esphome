#pragma once

#include "esphome/components/sensor/sensor.h"
#include "myair.h"

namespace esphome::myair {

enum class SensorType { TEMPERATURE, DAMPER, SENSOR_REPORT, SENSOR_ERROR, WRITE_FAILURES };

class MyAirSensor : public sensor::Sensor {
 public:
  MyAirSensor(MyAir *parent, SensorType type) : parent_(parent), type_(type) {
    parent->add_on_state_callback([this]() { refresh(); });
  }
  MyAirSensor(Zone *zone, SensorType type) : MyAirSensor(zone->parent, type) { zone_ = zone->number; }
  void refresh();

 protected:
  MyAir *parent_;
  uint8_t zone_{};
  SensorType type_;
};

}  // namespace esphome::myair
