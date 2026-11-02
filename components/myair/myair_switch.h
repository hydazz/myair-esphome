#pragma once

#include "esphome/components/switch/switch.h"
#include "myair.h"

namespace esphome::myair {

class ZoneSwitch : public switch_::Switch {
 public:
  explicit ZoneSwitch(Zone *zone) : zone_(zone) {
    zone_->parent->add_on_state_callback(
        [this]() { publish_state(zone_->parent->session().pairing_zone() == zone_->number); });
  }

 protected:
  void write_state(bool state) override { zone_->parent->pair_sensor(zone_->number, state); }
  Zone *zone_;
};

}  // namespace esphome::myair
