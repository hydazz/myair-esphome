#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "myair.h"

namespace esphome::myair {

class SystemBinarySensor : public binary_sensor::BinarySensor {
 public:
  explicit SystemBinarySensor(MyAir *parent) {
    parent->add_on_state_callback([this, parent]() { publish_state(parent->session().ready()); });
  }
};

class ZoneLowBattery : public binary_sensor::BinarySensor {
 public:
  explicit ZoneLowBattery(Zone *zone) {
    zone->parent->add_on_state_callback([this, zone]() {
      const auto flags = zone->parent->session().sensor_error(zone->number);
      if (flags.has_value())
        publish_state((*flags & field::CHECK_BATTERY) != 0);
      else
        invalidate_state();
    });
  }
};

}  // namespace esphome::myair
