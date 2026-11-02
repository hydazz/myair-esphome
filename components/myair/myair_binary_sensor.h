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

}  // namespace esphome::myair
