#pragma once

#include "esphome/components/number/number.h"
#include "myair.h"

namespace esphome::myair {

enum class NumberType { TEMPERATURE_OFFSET, MIN_DAMPER, MAX_DAMPER };

class ZoneNumber : public number::Number {
 public:
  ZoneNumber(Zone *zone, NumberType type) : zone_(zone), type_(type) {
    zone_->parent->add_on_state_callback([this]() { refresh(); });
  }
  void refresh();

 protected:
  void control(float value) override;
  Zone *zone_;
  NumberType type_;
};

}  // namespace esphome::myair
