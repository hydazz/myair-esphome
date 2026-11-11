#include <cmath>
#include <limits>
#include <utility>

#include "activity.h"
#include "test_support.h"

static EquipmentState equipment(Mode mode = Mode::COOL, bool enabled = true) { return {enabled, mode, Fan::LOW, 1}; }

static void confirm_power(Activity &activity, float watts, uint32_t start) {
  activity.update_power(watts, start);
  activity.update_power(watts, start + 5000);
}

void test_activity() {
  const std::pair<Mode, Action> modes[] = {{Mode::COOL, Action::COOLING},
                                           {Mode::HEAT, Action::HEATING},
                                           {Mode::DRY, Action::DRYING},
                                           {Mode::FAN, Action::FAN}};
  Activity activity;
  CHECK(activity.feedback() == Feedback::NOT_CONFIGURED);
  CHECK(!activity.system({}));
  CHECK(!activity.system(equipment(Mode::AUTO)));
  CHECK(!activity.system({EquipmentState{true, {}, {}, 0}}));
  for (const auto &[mode, action] : modes) CHECK(activity.system(equipment(mode)) == action);

  activity.set_power_config({50, 5000, 30000});
  CHECK(activity.feedback() == Feedback::WAITING);
  activity.update_power(15, 0);
  activity.tick(5000);
  CHECK(activity.system(equipment()) == Action::COOLING);
  activity.update_power(15, 5000);
  CHECK(activity.feedback() == Feedback::POWER);
  CHECK(activity.system(equipment(Mode::AUTO)) == Action::IDLE);
  CHECK(!activity.system({EquipmentState{true, {}, {}, 0}}));
  CHECK(!activity.system({}));

  // All operating modes use the same idle check; exactly the threshold uses mode.
  uint32_t now = 6000;
  for (float watts : {0.0f, 49.9f, 50.0f, 50.1f, 1000.0f}) {
    confirm_power(activity, watts, now);
    for (const auto &[mode, action] : modes) {
      CHECK(activity.system(equipment(mode)) == (watts < 50 ? Action::IDLE : action));
      CHECK(activity.system(equipment(mode, false)) == Action::OFF);
    }
    CHECK(activity.system(equipment(Mode::OFF)) == Action::OFF);
    CHECK(activity.system(equipment(Mode::AUTO)) == (watts < 50 ? std::optional<Action>{Action::IDLE} : std::nullopt));
    now += 6000;
  }

  Activity settling;
  settling.set_power_config({50, 5000, 30000});
  confirm_power(settling, 49, 0);
  settling.update_power(51, 6000);
  settling.update_power(49, 10000);
  settling.update_power(51, 11000);
  settling.update_power(51, 15999);
  CHECK(settling.system(equipment()) == Action::IDLE);
  settling.update_power(51, 16000);
  CHECK(settling.system(equipment()) == Action::COOLING);
  settling.update_power(49, 17000);
  settling.update_power(50, 21000);
  settling.update_power(49, 22000);
  settling.update_power(49, 26999);
  CHECK(settling.system(equipment()) == Action::COOLING);
  settling.update_power(49, 27000);
  CHECK(settling.system(equipment()) == Action::IDLE);
  CHECK(!settling.tick(56999));
  CHECK(settling.tick(57000));
  CHECK(settling.feedback() == Feedback::STALE);
  for (const auto &[mode, action] : modes) CHECK(settling.system(equipment(mode)) == action);
  CHECK(!settling.tick(58000));
  settling.update_power(49, 58000);
  CHECK(settling.feedback() == Feedback::WAITING);
  CHECK(settling.system(equipment()) == Action::COOLING);
  settling.update_power(49, 63000);
  CHECK(settling.system(equipment()) == Action::IDLE);

  now = 64000;
  for (float invalid : {NAN, INFINITY, -1.0f}) {
    settling.update_power(invalid, now);
    CHECK(settling.feedback() == Feedback::UNAVAILABLE);
    for (const auto &[mode, action] : modes) CHECK(settling.system(equipment(mode)) == action);
    settling.update_power(15, now + 1000);
    CHECK(settling.feedback() == Feedback::WAITING);
    CHECK(settling.system(equipment()) == Action::COOLING);
    settling.update_power(15, now + 6000);
    CHECK(settling.system(equipment()) == Action::IDLE);
    now += 7000;
  }

  // A sample after a long gap cannot revive an expired Idle, even before tick runs.
  settling.update_power(15, now + 30000);
  CHECK(settling.feedback() == Feedback::WAITING);
  CHECK(settling.system(equipment()) == Action::COOLING);

  const uint32_t start = std::numeric_limits<uint32_t>::max() - 2000;
  Activity wrapping;
  wrapping.set_power_config({50, 5000, 30000});
  confirm_power(wrapping, 15, start);
  CHECK(wrapping.system(equipment()) == Action::IDLE);
  wrapping.update_power(15, start + 30000);
  CHECK(!wrapping.tick(start + 59999));
  CHECK(wrapping.tick(start + 60000));

  CHECK(Activity::zone(Action::COOLING, true, 100) == Action::COOLING);
  CHECK(Activity::zone(Action::COOLING, true, 0) == Action::IDLE);
  CHECK(Activity::zone(Action::COOLING, false, 100) == Action::OFF);
  CHECK(Activity::zone(Action::OFF, true, 100) == Action::OFF);
  CHECK(!Activity::zone({}, true, 100));
  CHECK(!Activity::zone({}, false, 0));
  for (const auto action : {Action::COOLING, Action::HEATING, Action::DRYING}) {
    CHECK(Activity::zone(action, true, 5) == Action::IDLE);
    CHECK(Activity::zone(action, true, 10) == action);
    CHECK(!Activity::zone(action, true, NAN));
  }
  CHECK(Activity::zone(Action::FAN, true, 5) == Action::FAN);
  CHECK(Activity::zone(Action::FAN, true, 0) == Action::IDLE);
  confirm_power(activity, 15, now);
  for (const auto &[mode, action] : modes) {
    CHECK(Activity::zone(activity.system(equipment(mode)), true, 100) == Action::IDLE);
  }
}
