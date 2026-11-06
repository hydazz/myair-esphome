#include "controller.h"

#include <algorithm>
#include <cmath>

namespace esphome::myair {

static constexpr uint8_t MODE_CODES[] = {0, 0, 1, 2, 3, 5};
// Zone setpoints are whole degrees (a 20.5 °C write reads back as 20 °C).
static constexpr float SETPOINT_STEP = 1.0f;
// A room calls on Auto's side once it is this far past its edge, and Auto changes side once a room is this far past
// its other edge and none has called for changeover_hold (°C).
static constexpr float CALL = 0.5f;
static constexpr float CHANGEOVER = 1.0f;
// A room's range is at least this wide, so Auto's changeover points, CHANGEOVER past each edge, sit 4 °C apart and
// Auto doesn't swing between heating and cooling (°C).
static constexpr float MIN_RANGE = 2.0f;
// A room is behind once it is this far past its edge: the reference can move to it, and it earns a trim (°C).
static constexpr float BEHIND = 0.3f;
// The most a zone's setpoint is trimmed past its room's edge, and how far back inside the edge its room must come
// before the trim comes off (°C).
static constexpr float TRIM_LIMIT = 1.0f;
static constexpr float TRIM_RELEASE = 0.2f;
// The control box holds the reference zone's damper at its maximum, the system's one guaranteed air path. A zone
// carries the AC on its own with more than this share of the largest zone's airflow at its maximum damper; less is a
// zone the installer throttled down, such as a toilet, which only gets air alongside a larger one.
static constexpr float CARRIES = 0.15f;
// Fan Auto runs medium only with this share of the system's airflow open at the control box's damper positions, and
// high only with this much. More fan into a few small ducts adds pressure rather than air.
static constexpr float MEDIUM_OPEN = 0.1f;
static constexpr float HIGH_OPEN = 0.4f;
// Fan Auto's medium and high thresholds, as shares of the system's airflow, the margin around them, and the most a
// room's demand counts (°C).
static constexpr float FAN_MEDIUM = 0.15f;
static constexpr float FAN_HIGH = 0.35f;
static constexpr float FAN_MARGIN = 0.05f;
static constexpr float FAN_REACH = 2.0f;
// A candidate this far past its edge (°C) asks for medium or high on its own, with the margin around them.
static constexpr float FAN_MEDIUM_PAST = 1.0f;
static constexpr float FAN_HIGH_PAST = 2.0f;
static constexpr float FAN_PAST_MARGIN = 0.2f;

static float or_lowest(float value) { return std::isnan(value) ? -INFINITY : value; }

uint8_t Controller::zone_count_() const {
  const auto *config = session_.bank().find(kind::CONFIG);
  return config == nullptr ? 0 : std::min<uint8_t>(config->data[field::ZONE_COUNT], 10);
}

bool Controller::sensor_zone_(uint8_t zone) const {
  const auto *state = session_.bank().find(kind::ZONE, zone);
  return zone > 0 && zone <= zone_count_() && state != nullptr && state->data[field::SENSOR_TYPE] >= 1 &&
         state->data[field::SENSOR_TYPE] <= 4;
}

bool Controller::open_(uint8_t zone) const {
  const auto *state = session_.bank().find(kind::ZONE, zone);
  return state != nullptr && (state->data[field::DAMPER] & field::OPEN) != 0;
}

float Controller::temperature(uint8_t zone) const {
  const auto *state = session_.bank().find(kind::ZONE, zone);
  if (!sensor_zone_(zone) || state->data[field::WHOLE_DEGREES] == 0xFF || state->data[field::TENTHS] > 9) return NAN;
  return state->data[field::WHOLE_DEGREES] + state->data[field::TENTHS] / 10.0f + offset(zone);
}

// How far a room is past the edge the AC works it toward: positive while it needs the AC.
float Controller::error_(uint8_t zone, Side side) const {
  const auto &room = *rooms_[zone];
  const float temperature = this->temperature(zone);
  return side == Side::COOL ? temperature - room.cool_to : room.heat_to - temperature;
}

// The zone's airflow at its maximum damper.
float Controller::weight_(uint8_t zone) const {
  const auto *limits = session_.bank().find(kind::LIMITS, zone);
  return airflow_weight(zone) * (limits != nullptr ? limits->data[field::MAX_DAMPER] : 100) / 100.0f;
}

static bool normalize_room(Room &room, const std::optional<Room> &old) {
  room.heat_to = std::round(room.heat_to);
  room.cool_to = std::round(room.cool_to);
  if (!(room.heat_to >= MIN_SETPOINT) || !(room.cool_to <= MAX_SETPOINT) || room.heat_to > room.cool_to) return false;
  // A narrower range widens away from the edge that was set: heat-to holds unless only cool-to changed. Against a
  // setpoint limit, the edge that was set gives way instead.
  if (room.cool_to - room.heat_to < MIN_RANGE) {
    if (old && old->heat_to == room.heat_to && old->cool_to != room.cool_to) {
      room.cool_to = std::max(room.cool_to, MIN_SETPOINT + MIN_RANGE);
      room.heat_to = room.cool_to - MIN_RANGE;
    } else {
      room.heat_to = std::min(room.heat_to, MAX_SETPOINT - MIN_RANGE);
      room.cool_to = room.heat_to + MIN_RANGE;
    }
  }
  return true;
}

bool Controller::set_room(uint8_t zone, Room room) {
  if (zone == 0 || zone > 10 || !normalize_room(room, rooms_[zone])) return false;
  rooms_[zone] = room;
  return true;
}

std::optional<Room> Controller::room(uint8_t zone) const {
  if (zone > 10) return {};
  return rooms_[zone];
}

bool Controller::set_airflow(uint8_t zone, Airflow airflow) {
  if (zone == 0 || zone > 10 || airflow.position < 5 || airflow.position > 100 || airflow.position % 5 != 0)
    return false;
  airflows_[zone] = airflow;
  return true;
}

std::optional<Airflow> Controller::airflow(uint8_t zone) const {
  if (zone > 10) return {};
  return airflows_[zone];
}

bool Controller::set_ac_setpoint(float setpoint) {
  setpoint = std::round(setpoint);
  if (!(setpoint >= MIN_SETPOINT) || !(setpoint <= MAX_SETPOINT)) return false;
  ac_setpoint_ = setpoint;
  return true;
}

bool Controller::set_target(float target) {
  target = std::round(target);
  if (!(target >= MIN_SETPOINT) || !(target <= MAX_SETPOINT) ||
      (mode_ != Mode::HEAT && mode_ != Mode::COOL && mode_ != Mode::DRY && !(mode_ == Mode::AUTO && !target_room())))
    return false;
  if (mode_ == Mode::HEAT)
    set_targets(target, {});
  else
    set_targets({}, target);
  ac_setpoint_ = target;
  return true;
}

std::optional<Room> Controller::target_room() const {
  const uint8_t zone = reference();
  if (zone <= 10 && rooms_[zone]) return rooms_[zone];
  for (const auto &room : rooms_) {
    if (room) return room;
  }
  return {};
}

bool Controller::set_targets(std::optional<float> low, std::optional<float> high) {
  if ((!low && !high) || !target_room()) return false;
  for (auto *value : {&low, &high}) {
    if (!*value) continue;
    **value = std::round(**value);
    if (!(**value >= MIN_SETPOINT) || !(**value <= MAX_SETPOINT)) return false;
  }
  if (low && high) {
    Room range{false, *low, *high};
    if (!normalize_room(range, target_room())) return false;
    low = range.heat_to;
    high = range.cool_to;
  }
  for (auto &room : rooms_) {
    if (!room) continue;
    if (low && high) {
      room->heat_to = *low;
      room->cool_to = *high;
    } else if (low) {
      room->heat_to = std::min(*low, MAX_SETPOINT - MIN_RANGE);
      room->cool_to = std::max(room->cool_to, room->heat_to + MIN_RANGE);
    } else {
      room->cool_to = std::max(*high, MIN_SETPOINT + MIN_RANGE);
      room->heat_to = std::min(room->heat_to, room->cool_to - MIN_RANGE);
    }
  }
  if (mode_ == Mode::HEAT && low) ac_setpoint_ = *low;
  if ((mode_ == Mode::COOL || mode_ == Mode::DRY) && high) ac_setpoint_ = *high;
  return true;
}

float Controller::target() const {
  const uint8_t zone = reference();
  if (!sensor_zone_(zone)) return ac_setpoint_;
  if (!rooms_[zone]) return setpoint(zone);
  const bool heating = mode_ == Mode::HEAT || (mode_ == Mode::AUTO && auto_side_ == Side::HEAT);
  return heating ? rooms_[zone]->heat_to : rooms_[zone]->cool_to;
}

bool Controller::set_airflow_weight(uint8_t zone, float weight) {
  if (zone == 0 || zone > 10 || !(weight > 0) || !std::isfinite(weight)) return false;
  airflow_weights_[zone] = weight;
  return true;
}

uint8_t Controller::reference() const {
  return session_.ready() ? session_.bank().find(kind::SYSTEM)->data[field::REFERENCE] : 0;
}

float Controller::setpoint(uint8_t zone) const {
  const auto *state = session_.bank().find(kind::ZONE, zone);
  return sensor_zone_(zone) ? state->data[field::SETPOINT] / 2.0f + offset(zone) : NAN;
}

float Controller::damper(uint8_t zone) const {
  const auto *state = session_.bank().find(kind::ZONE, zone);
  if (!session_.ready() || zone == 0 || zone > zone_count_() || state == nullptr) return NAN;
  const uint8_t position = state->data[field::DAMPER] & field::POSITION;
  if (position > 100) return NAN;
  return state->data[field::DAMPER] & field::OPEN ? position : 0;
}

std::optional<Mode> Controller::ac_mode() const {
  if (!session_.ready()) return {};
  const auto &system = session_.bank().find(kind::SYSTEM)->data;
  if (system[field::POWER] == 0) return Mode::OFF;
  static constexpr Mode MODES[] = {Mode::OFF, Mode::COOL, Mode::HEAT, Mode::FAN, Mode::AUTO, Mode::DRY};
  const uint8_t code = system[field::MODE];
  if (code == 0 || code > 5) return {};
  return MODES[code];
}

std::optional<Fan> Controller::ac_fan() const {
  if (!session_.ready()) return {};
  static constexpr Fan FANS[] = {Fan::AUTO, Fan::LOW, Fan::MEDIUM, Fan::HIGH, Fan::AUTO};
  const uint8_t code = session_.bank().find(kind::SYSTEM)->data[field::FAN];
  if (code == 0 || code > 4) return {};
  return FANS[code];
}

// What the control box is doing for a zone: its damper opens past the floor while the zone is on the far side of its
// setpoint.
Action Controller::zone_action_(uint8_t zone) const {
  if (!session_.ready() || session_.bank().find(kind::SYSTEM)->data[field::POWER] == 0) return Action::OFF;
  switch (session_.bank().find(kind::SYSTEM)->data[field::MODE]) {
    case 1:
      return temperature(zone) > setpoint(zone) ? Action::COOLING : Action::IDLE;
    case 2:
      return temperature(zone) < setpoint(zone) ? Action::HEATING : Action::IDLE;
    case 3:
      return Action::FAN;
    case 5:
      return Action::DRYING;
    default:
      return Action::IDLE;
  }
}

Action Controller::action(uint8_t zone) const { return on_(zone) && open_(zone) ? zone_action_(zone) : Action::OFF; }

Action Controller::ac_action() const {
  if (reference() != 0) return zone_action_(reference());
  // On its own setpoint the AC's room temperature isn't on the bus, so only its mode shows.
  if (!session_.ready() || session_.bank().find(kind::SYSTEM)->data[field::POWER] == 0) return Action::OFF;
  static constexpr Action ACTIONS[] = {Action::IDLE, Action::COOLING, Action::HEATING,
                                       Action::FAN,  Action::IDLE,    Action::DRYING};
  const uint8_t mode = session_.bank().find(kind::SYSTEM)->data[field::MODE];
  return mode <= 5 ? ACTIONS[mode] : Action::IDLE;
}

bool Controller::set_offset(uint8_t zone, float value) {
  if (zone == 0 || zone > 10 || !std::isfinite(value) || value < -5 || value > 5) return false;
  offsets_[zone] = value;
  return true;
}

float Controller::damper_limit(uint8_t zone, bool maximum) const {
  const auto *limits = session_.bank().find(kind::LIMITS, zone);
  return session_.ready() && limits != nullptr ? limits->data[maximum ? field::MAX_DAMPER : field::MIN_DAMPER] : NAN;
}

bool Controller::set_damper_limit(uint8_t zone, bool maximum, float value) {
  if (!session_.ready() || zone == 0 || zone > zone_count_() || !std::isfinite(value) || value < 0 || value > 100 ||
      std::fmod(value, 5.0f) != 0)
    return false;
  const auto limits = session_.desired(kind::LIMITS, zone);
  if (!limits) return false;
  const auto minimum = maximum ? limits->data[field::MIN_DAMPER] : static_cast<uint8_t>(value);
  const auto max = maximum ? static_cast<uint8_t>(value) : limits->data[field::MAX_DAMPER];
  if (minimum > max) return false;
  Change change{kind::LIMITS, zone};
  change.set(maximum ? field::MAX_DAMPER : field::MIN_DAMPER, static_cast<uint8_t>(value));
  return session_.queue(change);
}

// Auto starts on the side with more airflow-weighted demand, so a small cold room doesn't start the house heating
// while large rooms want cooling. It stays on a side until no room has called on it for changeover_hold and a room
// wants the other side. Only rooms that carry the AC count: a throttled zone follows the AC through leakage, past its
// other edge, whatever its own needs.
Controller::Side Controller::choose_side_(uint32_t now) {
  if (mode_ == Mode::HEAT) return Side::HEAT;
  if (mode_ != Mode::AUTO) return Side::COOL;
  float cool = -INFINITY, heat = -INFINITY, cool_vote = 0, heat_vote = 0;
  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    if (!carrying_(zone)) continue;
    const float cooling = or_lowest(error_(zone, Side::COOL));
    const float heating = or_lowest(error_(zone, Side::HEAT));
    cool = std::max(cool, cooling);
    heat = std::max(heat, heating);
    cool_vote += weight_(zone) * std::max(cooling, 0.0f);
    heat_vote += weight_(zone) * std::max(heating, 0.0f);
  }
  if (!auto_side_) {
    const auto &system = session_.bank().find(kind::SYSTEM)->data;
    const bool heating = heat_vote > cool_vote || (heat_vote == cool_vote && heat > cool);
    auto_side_ = heating ? Side::HEAT : Side::COOL;
    // Keep the side the AC is already running unless nothing calls on it and a room wants the other.
    if (system[field::POWER] == 1 && (system[field::MODE] == 1 || system[field::MODE] == 2)) {
      const bool running_cool = system[field::MODE] == 1;
      if ((running_cool ? cool : heat) > CALL || (running_cool ? heat : cool) < CHANGEOVER)
        auto_side_ = running_cool ? Side::COOL : Side::HEAT;
    }
    last_call_ = now;
  }
  const bool cooling = *auto_side_ == Side::COOL;
  if ((cooling ? cool : heat) > CALL) last_call_ = now;
  if (now - last_call_ >= tuning.changeover_hold && (cooling ? heat : cool) >= CHANGEOVER) {
    auto_side_ = cooling ? Side::HEAT : Side::COOL;
    last_call_ = now;
  }
  return *auto_side_;
}

bool Controller::carries_(uint8_t zone) const {
  float largest = 0;
  for (uint8_t other = 1; other <= zone_count_(); other++) largest = std::max(largest, weight_(other));
  return weight_(zone) > CARRIES * largest;
}

bool Controller::carrying_(uint8_t zone) const { return on_(zone) && sensor_zone_(zone) && carries_(zone); }

bool Controller::candidate_(uint8_t zone) const { return carrying_(zone) && open_(zone); }

// The control box's damper loop is proportional: a room with more load than its share of air settles past its edge, its
// damper part open. An open zone other than the reference whose room is past BEHIND gets a trim, its zone setpoint a
// step past its room's edge, and loses it once its room is TRIM_RELEASE back inside, each at most once per trim_hold.
// Its damper then opens before the room drifts.
void Controller::trim_(uint32_t now, Side side, uint8_t reference) {
  const bool conditioning = mode_ == Mode::AUTO || mode_ == Mode::COOL || mode_ == Mode::HEAT;
  if (side != trim_side_) {
    trim_side_ = side;
    trims_.fill(0);
    trim_changed_.fill(now);
  }
  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    const float error = conditioning && zone != reference && on_(zone) && open_(zone) ? error_(zone, side) : NAN;
    if (std::isnan(error)) {
      trims_[zone] = 0;
      trim_changed_[zone] = now;
      continue;
    }
    if (now - trim_changed_[zone] < tuning.trim_hold) continue;
    if (error > BEHIND && trims_[zone] < TRIM_LIMIT) {
      trims_[zone] += SETPOINT_STEP;
    } else if (error < -TRIM_RELEASE && trims_[zone] > 0) {
      trims_[zone] -= SETPOINT_STEP;
    } else {
      continue;
    }
    trim_changed_[zone] = now;
  }
}

// The AC regulates on the reference zone alone and backs off once that room is satisfied, however far another room
// is past its edge. The reference starts in the candidate furthest past its edge, the larger airflow on a tie. Once
// its room is satisfied and another candidate is behind, it moves to the candidate furthest past its edge, at most once
// per reference_hold; a room past its edge keeps the AC working through the move. It moves at once when its room
// turns off.
uint8_t Controller::choose_reference_(uint32_t now, Side side) const {
  uint8_t best = 0;
  float highest = -INFINITY;
  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    if (!candidate_(zone)) continue;
    const float error = or_lowest(error_(zone, side));
    if (best == 0 || error > highest || (error == highest && weight_(zone) > weight_(best))) {
      best = zone;
      highest = error;
    }
  }
  const uint8_t current = session_.bank().find(kind::SYSTEM)->data[field::REFERENCE];
  if (!candidate_(current)) return best;
  if (best != current && highest > BEHIND && !(error_(current, side) > 0) &&
      now - reference_changed_ >= tuning.reference_hold)
    return best;
  return current;
}

bool Controller::constant_(uint8_t zone) const {
  const auto *config = session_.bank().find(kind::CONFIG);
  if (config == nullptr || zone == 0) return false;
  for (uint8_t i = 0; i < std::min<uint8_t>(config->data[field::CONSTANT_COUNT], 3); i++) {
    if (config->data[field::FIRST_CONSTANT + i] == zone) return true;
  }
  return false;
}

// Installer constant zones hold up the system's airflow: the touch screen opens them while the other open zones' damper
// positions add up to less than 100 % per constant zone. Returns the missing percentage.
uint8_t Controller::shortfall_() const {
  const auto *config = session_.bank().find(kind::CONFIG);
  if (config == nullptr || config->data[field::CONSTANT_COUNT] == 0) return 0;
  int open = 0;
  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    const auto *state = session_.bank().find(kind::ZONE, zone);
    if (state != nullptr && !constant_(zone) && (state->data[field::DAMPER] & field::OPEN))
      open += state->data[field::DAMPER] & field::POSITION;
  }
  const int floor = std::min<uint8_t>(config->data[field::CONSTANT_COUNT], 3) * 100;
  return open < floor ? floor - open : 0;
}

// Fan Auto matches the airflow the zones ask for: each open room's share of the system's airflow, scaled by how far
// it is past its edge up to FAN_REACH, and each open airflow zone's share at its position. The control box opens a
// damper fully at 1 °C past; beyond that only more air helps, so the candidate furthest past its edge also steps the
// fan up on its own. It moves one speed at a time, except that it drops at once to what the open dampers can take.
uint8_t Controller::choose_fan_(uint32_t now, Side side) const {
  if (fan_ != Fan::AUTO) return static_cast<uint8_t>(fan_);
  const auto &system = session_.bank().find(kind::SYSTEM)->data;
  const bool running = system[field::POWER] == 1 && system[field::FAN] >= 1 && system[field::FAN] <= 3;
  const uint8_t current = running ? system[field::FAN] : 1;
  float asked = 0, capacity = 0, open = 0;
  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    capacity += weight_(zone);
    // The dampers as this batch leaves them: its zone writes are queued before the fan is chosen.
    const auto state = session_.desired(kind::ZONE, zone);
    if (state && (state->data[field::DAMPER] & field::OPEN))
      open += weight_(zone) * (state->data[field::DAMPER] & field::POSITION) / 100.0f;
    if (on_(zone) && open_(zone) && sensor_zone_(zone))
      asked += weight_(zone) * std::clamp(or_lowest(error_(zone, side)), 0.0f, FAN_REACH);
    if (airflow_on_(zone) && open_(zone)) asked += weight_(zone) * airflows_[zone]->position / 100.0f;
  }
  uint8_t fan = 1;
  if ((mode_ == Mode::AUTO || mode_ == Mode::COOL || mode_ == Mode::HEAT) && capacity > 0) {
    const float share = asked / capacity;
    // A candidate with its damper full gets more air only from more fan.
    float furthest = -INFINITY;
    for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
      if (candidate_(zone)) furthest = std::max(furthest, or_lowest(error_(zone, side)));
    }
    if (share >= FAN_HIGH + (current == 3 ? -FAN_MARGIN : FAN_MARGIN) ||
        furthest >= FAN_HIGH_PAST + (current == 3 ? -FAN_PAST_MARGIN : FAN_PAST_MARGIN))
      fan = 3;
    else if (share >= FAN_MEDIUM + (current >= 2 ? -FAN_MARGIN : FAN_MARGIN) ||
             furthest >= FAN_MEDIUM_PAST + (current >= 2 ? -FAN_PAST_MARGIN : FAN_PAST_MARGIN))
      fan = 2;
  }
  // Rising to a speed takes its whole share open; staying on it, three quarters.
  const auto allows = [&](uint8_t speed, float share) {
    return open >= share * (current >= speed ? 0.75f : 1) * capacity;
  };
  const uint8_t limit = allows(3, HIGH_OPEN) ? 3 : allows(2, MEDIUM_OPEN) ? 2 : 1;
  fan = std::min(fan, limit);
  if (!running) return fan;
  if (current > limit) return limit;
  if (fan == current) return current;
  // Up after fan_hold, down after twice that, so the fan doesn't follow every on-off cycle of the AC.
  if (now - fan_changed_ < (fan > current ? tuning.fan_hold : 2 * tuning.fan_hold)) return current;
  return fan > current ? current + 1 : current - 1;
}

void Controller::observe_(uint32_t now) {
  const auto &system = session_.bank().find(kind::SYSTEM)->data;
  if (!observed_) {
    fan_changed_ = now - 2 * tuning.fan_hold;
    reference_changed_ = now - tuning.reference_hold;
  } else {
    if (observed_fan_ != system[field::FAN]) fan_changed_ = now;
    if (observed_reference_ != system[field::REFERENCE]) reference_changed_ = now;
  }
  observed_ = true;
  observed_fan_ = system[field::FAN];
  observed_reference_ = system[field::REFERENCE];
}

void Controller::step(uint32_t now) {
  if (generation_ != session_.generation()) {
    generation_ = session_.generation();
    observed_ = false;
  }
  if (mode_ != Mode::AUTO) auto_side_.reset();
  if (!session_.ready()) return;
  observe_(now);
  // Each batch is planned from confirmed state, so a zone opens before it becomes the reference and the reference
  // moves before its zone closes.
  if (session_.writes_pending()) return;

  const auto &system = session_.bank().find(kind::SYSTEM)->data;
  const Side side = choose_side_(now);
  trim_(now, side, system[field::REFERENCE]);

  const bool setpoints = mode_ != Mode::OFF && mode_ != Mode::FAN;
  // A room that carries the AC gives it a reference; with none on, airflow zones run it on its own setpoint. Smaller
  // rooms only get air alongside those.
  bool any_on = false, rooms_on = false;
  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    any_on |= carrying_(zone) || airflow_on_(zone);
    rooms_on |= carrying_(zone);
  }
  const bool running = mode_ != Mode::OFF && any_on;
  const uint8_t mode =
      mode_ == Mode::AUTO ? (rooms_on ? (side == Side::COOL ? 1 : 2) : 4) : MODE_CODES[static_cast<uint8_t>(mode_)];
  if (!running) {
    if (system[field::POWER] != 0) {
      Change stop{kind::SYSTEM};
      stop.set(field::POWER, 0);
      session_.queue(stop);
    }
    // Power-off readback is not fan-stop feedback. Leave the reference and damper commands alone during shutdown
    // and while off; room requests are applied when starting again.
    return;
  }
  const uint8_t shortfall = shortfall_();
  const auto *config = session_.bank().find(kind::CONFIG);
  const uint8_t constants = config == nullptr ? 0 : std::clamp<uint8_t>(config->data[field::CONSTANT_COUNT], 1, 3);

  for (uint8_t zone = 1; zone <= zone_count_(); zone++) {
    if (!rooms_[zone] && !airflows_[zone]) continue;
    const bool forced = shortfall > 0 && constant_(zone);
    Change change{kind::ZONE, zone};
    if (airflows_[zone]) {
      uint8_t position = airflow_on_(zone) ? airflows_[zone]->position : 0;
      if (forced) position = std::max<uint8_t>(position, std::min(100, (shortfall / constants + 4) / 5 * 5));
      if (position > 0)
        change.set(field::DAMPER, field::OPEN | position);
      else
        change.set(field::DAMPER, 0, field::OPEN);
    } else if (on_(zone) || forced) {
      change.set(field::DAMPER, field::OPEN, field::OPEN);
      if (on_(zone) && setpoints && sensor_zone_(zone)) {
        const auto &room = *rooms_[zone];
        float edge = side == Side::COOL ? room.cool_to : room.heat_to;
        edge += side == Side::COOL ? -trims_[zone] : trims_[zone];
        change.set(field::SETPOINT,
                   static_cast<uint8_t>(std::clamp(std::round(edge - offset(zone)), MIN_SETPOINT, MAX_SETPOINT) * 2));
      }
    } else if (zone != system[field::REFERENCE]) {
      // Keep the current reference open until the new reference has been confirmed.
      change.set(field::DAMPER, 0, field::OPEN);
    }
    session_.queue(change);
  }

  const uint8_t reference = choose_reference_(now, side);
  if (rooms_on && reference == 0) return;
  Change change{kind::SYSTEM};
  change.set(field::POWER, 1);
  change.set(field::MODE, mode);
  change.set(field::FAN, choose_fan_(now, side));
  if (setpoints) {
    // The AC's setpoint follows the reference room's edge, for ACs that regulate on return air.
    float setpoint = ac_setpoint_;
    if (reference != 0) {
      const auto &room = *rooms_[reference];
      setpoint = std::round((side == Side::COOL ? room.cool_to : room.heat_to) - offset(reference));
    }
    change.set(field::AC_SETPOINT, static_cast<uint8_t>(std::clamp(setpoint, MIN_SETPOINT, MAX_SETPOINT) * 2));
  }
  change.set(field::REFERENCE, reference);
  session_.queue(change);
}

}  // namespace esphome::myair
