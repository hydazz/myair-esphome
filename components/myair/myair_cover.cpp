#include "myair_cover.h"

#include <algorithm>
#include <cmath>

namespace esphome::myair {

cover::CoverTraits ZoneCover::get_traits() {
  cover::CoverTraits traits;
  traits.set_supports_position(true);
  traits.set_supports_toggle(true);
  return traits;
}

void ZoneCover::control(const cover::CoverCall &call) {
  auto airflow = zone_->parent->controller().airflow(zone_->number).value_or(Airflow{});
  if (call.get_toggle()) airflow.on = !airflow.on;
  if (call.get_position()) {
    const float position = *call.get_position();
    // Opening a closed zone turns it on at its last position, as the touch screen does.
    if (!(position >= cover::COVER_OPEN && !airflow.on) && position > 0)
      airflow.position = std::clamp<int>(std::lround(position * 20) * 5, 5, 100);
    airflow.on = position > 0;
  }
  zone_->parent->set_airflow(zone_->number, airflow);
}

// The control box reports an airflow zone's damper as the position it holds; before it reports, the zone's own setting
// shows.
void ZoneCover::refresh() {
  const auto &controller = zone_->parent->controller();
  const auto airflow = controller.airflow(zone_->number).value_or(Airflow{});
  const float reported = controller.damper(zone_->number);
  const float next = !std::isnan(reported) ? reported / 100 : airflow.on ? airflow.position / 100.0f : 0;
  if (next == position) return;
  position = next;
  publish_state(false);
}

}  // namespace esphome::myair
