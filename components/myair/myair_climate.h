#pragma once

#include "esphome/components/climate/climate.h"
#include "myair.h"

namespace esphome::myair {

// The AC: mode, fan and target. Its temperature is the reference zone's and its target the edge it works to, which a
// write sets in every room. The hub keeps the state; this entity only shows and changes it.
class SystemClimate : public climate::Climate, public Component {
 public:
  explicit SystemClimate(MyAir *parent) : parent_(parent) {
    parent_->add_on_state_callback([this]() { refresh(); });
  }
  void setup() override { refresh(); }
  float get_setup_priority() const override { return setup_priority::DATA; }
  void refresh();

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  MyAir *parent_;
};

// A room: off or heat/cool with a range. The zone keeps the state; this entity only shows and changes it.
class ZoneClimate : public climate::Climate, public Component {
 public:
  explicit ZoneClimate(Zone *zone) : zone_(zone) {
    zone_->parent->add_on_state_callback([this]() { refresh(); });
  }
  void setup() override { refresh(); }
  float get_setup_priority() const override { return setup_priority::DATA; }
  void refresh();

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  Zone *zone_;
};

}  // namespace esphome::myair
