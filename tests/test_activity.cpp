#include <cmath>
#include <utility>

#include "activity.h"
#include "test_support.h"

static EquipmentState equipment(Mode mode = Mode::COOL, bool enabled = true) { return {enabled, mode, Fan::LOW, 1}; }

void test_activity() {
  Activity activity;
  CHECK(activity.feedback() == Feedback::NOT_CONFIGURED);
  for (const auto &[mode, action] : {std::pair{Mode::COOL, Action::COOLING},
                                     {Mode::HEAT, Action::HEATING},
                                     {Mode::DRY, Action::DRYING},
                                     {Mode::FAN, Action::FAN}})
    CHECK(activity.system(equipment(mode)) == action);
  CHECK(!activity.system(equipment(Mode::AUTO)));

  activity.set_idle_below(50);
  activity.update_power(15);
  CHECK(activity.feedback() == Feedback::POWER);
  CHECK(activity.system(equipment()) == Action::IDLE);
  CHECK(activity.system(equipment(Mode::AUTO)) == Action::IDLE);
  CHECK(activity.system(equipment(Mode::COOL, false)) == Action::OFF);
  CHECK(!activity.system({}));

  activity.update_power(50);
  CHECK(activity.system(equipment()) == Action::COOLING);
  CHECK(!activity.system(equipment(Mode::AUTO)));

  activity.update_power(0);
  CHECK(activity.system(equipment()) == Action::IDLE);
  activity.update_power(NAN);
  CHECK(activity.feedback() == Feedback::UNAVAILABLE);
  CHECK(activity.system(equipment()) == Action::COOLING);
  activity.update_power(15);
  CHECK(activity.system(equipment()) == Action::IDLE);

  CHECK(Activity::zone(Action::COOLING, true, 5) == Action::IDLE);
  CHECK(Activity::zone(Action::HEATING, true, 10) == Action::HEATING);
  CHECK(Activity::zone(Action::FAN, true, 5) == Action::FAN);
  CHECK(Activity::zone(Action::FAN, true, 0) == Action::IDLE);
  CHECK(Activity::zone(Action::COOLING, false, 100) == Action::OFF);
  CHECK(Activity::zone(Action::IDLE, true, 100) == Action::IDLE);
  CHECK(Activity::zone(Action::OFF, true, 100) == Action::OFF);
  CHECK(!Activity::zone({}, true, 100));
}
