#pragma once

#include "esphome/components/button/button.h"
#include "myair.h"

namespace esphome::myair {

class ZoneButton : public button::Button {
 public:
  explicit ZoneButton(Zone *zone) : zone_(zone) {}

 protected:
  void press_action() override { zone_->parent->unpair_sensor(zone_->number); }
  Zone *zone_;
};

}  // namespace esphome::myair
