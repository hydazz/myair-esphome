#pragma once

#include "esphome/components/cover/cover.h"
#include "myair.h"

namespace esphome::myair {

// A zone without a sensor: on at a damper position in 5 % steps, or off. Opening it restores its last position. The
// zone keeps the state; this entity only shows and changes it.
class ZoneCover : public cover::Cover, public Component {
 public:
  explicit ZoneCover(Zone *zone) : zone_(zone) {
    zone_->parent->add_on_state_callback([this]() { refresh(); });
  }
  void setup() override { refresh(); }
  float get_setup_priority() const override { return setup_priority::DATA; }
  void refresh();

 protected:
  cover::CoverTraits get_traits() override;
  void control(const cover::CoverCall &call) override;
  Zone *zone_;
};

}  // namespace esphome::myair
