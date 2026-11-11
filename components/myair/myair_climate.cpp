#include "myair_climate.h"

#include <cmath>

#include "esphome/core/controller_registry.h"

namespace esphome::myair {

static const climate::ClimateAction ACTIONS[] = {
    climate::CLIMATE_ACTION_OFF,     climate::CLIMATE_ACTION_IDLE, climate::CLIMATE_ACTION_COOLING,
    climate::CLIMATE_ACTION_HEATING, climate::CLIMATE_ACTION_FAN,  climate::CLIMATE_ACTION_DRYING,
};
static const climate::ClimateMode MODES[] = {
    climate::CLIMATE_MODE_OFF,  climate::CLIMATE_MODE_HEAT_COOL, climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_FAN_ONLY,  climate::CLIMATE_MODE_DRY,
};
static const climate::ClimateFanMode FANS[] = {
    climate::CLIMATE_FAN_AUTO,
    climate::CLIMATE_FAN_LOW,
    climate::CLIMATE_FAN_MEDIUM,
    climate::CLIMATE_FAN_HIGH,
};

static bool same(float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); }

static climate::ClimateTraits base_traits() {
  climate::ClimateTraits traits;
  traits.set_visual_min_temperature(MIN_SETPOINT);
  traits.set_visual_max_temperature(MAX_SETPOINT);
  traits.set_visual_target_temperature_step(1);
  traits.set_visual_current_temperature_step(0.1);
  return traits;
}

climate::ClimateTraits SystemClimate::traits() {
  auto traits = base_traits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  if (parent_->has_temperature_zones())
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT_COOL, climate::CLIMATE_MODE_COOL,
                              climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_DRY});
  traits.set_supported_fan_modes(
      {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_HIGH});
  return traits;
}

void SystemClimate::control(const climate::ClimateCall &call) {
  if (auto value = call.get_mode()) {
    for (size_t i = 0; i < 6; i++) {
      if (MODES[i] == *value) parent_->set_mode(static_cast<Mode>(i));
    }
  }
  if (auto value = call.get_fan_mode()) {
    for (size_t i = 0; i < 4; i++) {
      if (FANS[i] == *value) parent_->set_fan(static_cast<Fan>(i));
    }
  }
  if (auto target = call.get_target_temperature()) {
    parent_->set_target(*target);
  }
  if (call.get_target_temperature_low() || call.get_target_temperature_high()) {
    std::optional<float> low, high;
    if (call.get_target_temperature_low()) low = *call.get_target_temperature_low();
    if (call.get_target_temperature_high()) high = *call.get_target_temperature_high();
    parent_->set_targets(low, high);
  }
  parent_->refresh();
}

// The AC works on the reference zone's temperature and setpoint, or on its own setpoint and return air, which the
// control box doesn't report.
void SystemClimate::refresh() {
  auto &controller = parent_->controller();
  const auto reference = parent_->session().equipment().value_or(EquipmentState{}).reference;
  const auto next_mode = MODES[static_cast<uint8_t>(controller.mode())];
  const auto next_fan = FANS[static_cast<uint8_t>(controller.fan())];
  const auto activity = parent_->activity();
  if (!activity) {
    if (has_state()) {
      set_has_state(false);
      state_callback_.call(*this);
#ifdef USE_CONTROLLER_REGISTRY
      ControllerRegistry::notify_climate_update(this);
#endif
    }
    return;
  }
  const auto next_action = ACTIONS[static_cast<uint8_t>(*activity)];
  const float current = controller.temperature(reference);
  const float target = controller.target();
  const bool ranges = parent_->has_temperature_zones();
  const auto room = controller.target_room();
  const float low = room ? room->heat_to : NAN;
  const float high = room ? room->cool_to : NAN;
  const bool same_targets = ranges ? same(target_temperature_low, low) && same(target_temperature_high, high)
                                   : same(target_temperature, target);
  if (has_state() && mode == next_mode && fan_mode == next_fan && action == next_action &&
      same(current_temperature, current) && same_targets)
    return;
  mode = next_mode;
  fan_mode = next_fan;
  action = next_action;
  current_temperature = current;
  if (ranges) {
    target_temperature_low = low;
    target_temperature_high = high;
  } else {
    target_temperature = target;
  }
  publish_state();
}

climate::ClimateTraits ZoneClimate::traits() {
  auto traits = base_traits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE |
                           climate::CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT_COOL});
  return traits;
}

void ZoneClimate::control(const climate::ClimateCall &call) {
  auto room = zone_->parent->controller().room(zone_->number).value_or(Room{});
  if (call.get_mode()) room.on = *call.get_mode() != climate::CLIMATE_MODE_OFF;
  if (call.get_target_temperature_low()) room.heat_to = *call.get_target_temperature_low();
  if (call.get_target_temperature_high()) room.cool_to = *call.get_target_temperature_high();
  zone_->parent->set_room(zone_->number, room);
}

void ZoneClimate::refresh() {
  auto &controller = zone_->parent->controller();
  const auto room = controller.room(zone_->number).value_or(Room{});
  const auto current = controller.temperature(zone_->number);
  const auto next_mode = room.on ? climate::CLIMATE_MODE_HEAT_COOL : climate::CLIMATE_MODE_OFF;
  const auto activity = Activity::zone(zone_->parent->activity(), room.on, controller.damper(zone_->number));
  if (!activity) {
    if (has_state()) {
      set_has_state(false);
      state_callback_.call(*this);
#ifdef USE_CONTROLLER_REGISTRY
      ControllerRegistry::notify_climate_update(this);
#endif
    }
    return;
  }
  const auto next_action = ACTIONS[static_cast<uint8_t>(*activity)];
  if (has_state() && mode == next_mode && action == next_action && target_temperature_low == room.heat_to &&
      target_temperature_high == room.cool_to && same(current_temperature, current))
    return;
  mode = next_mode;
  action = next_action;
  target_temperature_low = room.heat_to;
  target_temperature_high = room.cool_to;
  current_temperature = current;
  publish_state();
}

}  // namespace esphome::myair
