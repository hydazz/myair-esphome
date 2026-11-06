#include <algorithm>
#include <cmath>

#include "test_support.h"

static void ready(Bus &bus, uint8_t count = 3) {
  start(bus);
  bus.receive(snapshot(count));
  CHECK(bus.ping() == "ackCAN 1");
}

static void report(Bus &bus, Record record) {
  record.direction = 3;
  bus.receive("getCAN 1 " + encode_record(record));
}

static std::vector<Record> next_writes(Bus &bus) {
  auto payload = bus.ping();
  if (payload == "ackCAN 1") payload = bus.ping();
  CHECK(payload.substr(0, 7) == "setCAN ");
  std::vector<Record> records;
  bool accepted;
  CHECK(parse_get_can("getCAN 1 " + payload.substr(7), accepted, records));
  CHECK(!records.empty());
  return records;
}

static Record next_write(Bus &bus) {
  const auto records = next_writes(bus);
  CHECK(records.size() == 1);
  return records.front();
}

static Record apply(Bus &bus, const Record &write) {
  const auto *observed = bus.session.bank().find(write.kind, write.kind == 3 || write.kind == 4 ? write.data[0] : 0);
  CHECK(observed != nullptr);
  auto result = *observed;
  if (write.kind == 3) {
    result.data[1] = write.data[1];
    result.data[3] = write.data[3];
  } else if (write.kind == 4) {
    result.data[1] = write.data[1];
    result.data[2] = write.data[2];
    result.data[4] = write.data[4];
  } else {
    CHECK(write.kind == 5);
    std::copy_n(write.data.begin(), 6, result.data.begin());
  }
  report(bus, result);
  return result;
}

static void test_confirmed_writes() {
  Bus bus;
  ready(bus);
  Change target{3, 2};
  target.set(3, 50);
  CHECK(bus.session.queue(target));
  CHECK(bus.session.bank().find(3, 2)->data[3] == 48);
  const auto write = next_write(bus);
  CHECK(encode_record(write) == "0701abcde0302b20032000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.session.writes_pending());
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN ");
  bus.now += 1000;
  CHECK(bus.ping() == "setCAN 0701000000600000000000000");
  auto echo = *bus.session.bank().find(3, 2);
  echo.data[1] = 0xE4;
  echo.data[3] = 50;
  echo.data[4] = 25;
  report(bus, echo);
  CHECK(!bus.session.writes_pending());
  CHECK(bus.session.bank().find(3, 2)->data[3] == 50);
  CHECK(bus.session.bank().find(3, 2)->data[4] == 25);
}

static void test_merge_and_preserve() {
  Bus bus;
  ready(bus);
  Change mode{5};
  mode.set(1, 2);
  CHECK(bus.session.queue(mode));
  Change fan{5};
  fan.set(2, 3);
  CHECK(bus.session.queue(fan));
  auto state = *bus.session.bank().find(5);
  state.data[4] = 3;
  state.data[5] = 2;
  state.data[6] = 0xAB;
  report(bus, state);
  const auto write = next_write(bus);
  CHECK(write.data == (std::array<uint8_t, 7>{1, 2, 3, 48, 3, 2, 0}));
  CHECK(bus.session.writes_pending());
  apply(bus, write);
  CHECK(!bus.session.writes_pending());
  CHECK(bus.session.bank().find(5)->data[6] == 0xAB);
}

static void test_new_command_during_readback() {
  Bus bus;
  ready(bus);
  Change target{3, 1};
  target.set(3, 50);
  CHECK(bus.session.queue(target));
  auto first = next_write(bus);
  target.set(3, 46);
  CHECK(bus.session.queue(target));
  target.set(3, 44);
  CHECK(bus.session.queue(target));
  CHECK(bus.session.desired(3, 1)->data[3] == 44);
  CHECK(bus.session.bank().find(3, 1)->data[3] == 48);
  apply(bus, first);
  CHECK(bus.session.bank().find(3, 1)->data[3] == 50);
  CHECK(bus.session.writes_pending());
  const auto second = next_write(bus);
  CHECK(second.data[3] == 44);
  apply(bus, second);
  CHECK(!bus.session.writes_pending());
}

static void test_wrong_readback_and_timeout() {
  Bus bus;
  ready(bus);
  Change mode{5};
  mode.set(1, 2);
  CHECK(bus.session.queue(mode));
  const auto write = next_write(bus);
  auto wrong = write;
  wrong.unit_id = 0x11111;
  report(bus, wrong);
  CHECK(bus.session.writes_pending());
  report(bus, *bus.session.bank().find(5));
  CHECK(bus.session.writes_pending());
  bus.now += 5000;
  bus.session.tick(bus.now);
  CHECK(!bus.session.ready());
  CHECK(!bus.session.writes_pending());
  CHECK(bus.session.write_failures() == 1);
  CHECK(bus.session.discarded_writes() == "unconfirmed AC settings " + encode_record(write));
  ready(bus);
  CHECK(bus.ping() == "setCAN ");
  CHECK(bus.session.bank().find(5)->data[1] == 1);
}

static void test_write_nack_and_crc_failure() {
  Bus bus;
  ready(bus, 1);
  Change power{5};
  power.set(0, 0);
  CHECK(bus.session.queue(power));
  const auto write = next_write(bus);
  bus.receive("getCAN 0 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == set_can({write}));
  auto echo = *bus.session.bank().find(5);
  echo.data[0] = 0;
  auto corrupt = frame("getCAN 1 " + encode_record(echo));
  corrupt[corrupt.size() - 2] = corrupt[corrupt.size() - 2] == '0' ? '1' : '0';
  CHECK(bus.wire(corrupt).empty());
  CHECK(bus.session.writes_pending());
  CHECK(bus.session.bank().find(5)->data[0] == 1);
  CHECK(bus.ping() == "ackCAN 0");
  CHECK(bus.ping() == set_can({write}));
  report(bus, echo);
  CHECK(!bus.session.writes_pending());
  CHECK(bus.session.bank().find(5)->data[0] == 0);
}

static void test_balancing_readback() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  CHECK(controller.damper_limit(1, false) == 0);
  CHECK(controller.damper_limit(1, true) == 100);
  CHECK(controller.set_damper_limit(1, true, 45));
  CHECK(controller.set_damper_limit(1, false, 5));
  // Limits move in 5 % steps, and the minimum stays at or under the maximum.
  CHECK(!controller.set_damper_limit(1, true, 49));
  CHECK(!controller.set_damper_limit(1, false, 50));
  CHECK(!controller.set_damper_limit(1, true, 0));
  CHECK(!controller.set_damper_limit(1, true, 101));
  CHECK(!controller.set_damper_limit(1, false, -1));
  CHECK(!controller.set_damper_limit(1, false, NAN));
  CHECK(!controller.set_damper_limit(1, false, 1.5));
  CHECK(!controller.set_damper_limit(0, true, 50));
  CHECK(!controller.set_damper_limit(3, true, 50));
  CHECK(controller.damper_limit(1, false) == 0);
  CHECK(controller.damper_limit(1, true) == 100);
  auto settings = *bus.session.bank().find(4, 1);
  settings.data[3] = 21;
  settings.data[4] = 2;
  settings.data[5] = 8;
  report(bus, settings);
  const auto write = next_write(bus);
  CHECK(encode_record(write) == "0701abcde0401052d00020000");
  bus.receive("getCAN 1 ");
  CHECK(bus.session.writes_pending());
  auto wrong_zone = write;
  wrong_zone.data[0] = 2;
  report(bus, wrong_zone);
  CHECK(bus.session.writes_pending());
  auto stale = settings;
  stale.data[1] = 1;
  report(bus, stale);
  CHECK(bus.session.writes_pending());
  apply(bus, write);
  CHECK(!bus.session.writes_pending());
  CHECK(controller.damper_limit(1, false) == 5);
  CHECK(controller.damper_limit(1, true) == 45);
  CHECK(bus.session.bank().find(4, 1)->data[3] == 21);
  CHECK(bus.session.bank().find(4, 1)->data[4] == 2);
  CHECK(bus.session.bank().find(4, 1)->data[5] == 8);
  CHECK(bus.session.bank().find(4, 1)->data[6] == 64);
}

static void set_temperature(Bus &bus, uint8_t zone, float value) {
  auto state = *bus.session.bank().find(3, zone);
  const auto tenths = static_cast<int>(std::lround(value * 10));
  state.data[4] = tenths / 10;
  state.data[5] = tenths % 10;
  report(bus, state);
}

static void set_open(Bus &bus, uint8_t zone, bool open) {
  auto state = *bus.session.bank().find(3, zone);
  state.data[1] = open ? state.data[1] | 0x80 : state.data[1] & 0x7F;
  report(bus, state);
}

// Steps the controller and lets the control box confirm every write until nothing is left to send.
static std::vector<Record> settle(Bus &bus, Controller &controller) {
  std::vector<Record> writes;
  for (int i = 0; i < 50; i++) {
    controller.step(bus.now);
    if (!bus.session.writes_pending()) return writes;
    while (bus.session.writes_pending()) {
      for (const auto &write : next_writes(bus)) {
        writes.push_back(write);
        apply(bus, write);
      }
    }
  }
  CHECK(false);
  return writes;
}

// Advances the clock with the link alive, stopping at the first write the controller queues.
static void wait(Bus &bus, Controller &controller, uint32_t ms) {
  for (uint32_t elapsed = 0; elapsed < ms; elapsed += 1000) {
    bus.now += 1000;
    bus.receive("getCAN 1 ");
    controller.step(bus.now);
    if (bus.session.writes_pending()) return;
  }
}

static void test_power_follows_rooms_and_unmanaged_zones() {
  Bus bus;
  ready(bus, 3);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(1, {false, 20, 24}));
  auto writes = settle(bus, controller);
  CHECK(writes.size() == 1);
  CHECK(writes[0].kind == 5 && writes[0].data[0] == 0);
  // Zone 1 is the reference, and zones 2 and 3 have no room.
  for (uint8_t zone : {1, 2, 3}) CHECK(bus.session.bank().find(3, zone)->data[1] & 0x80);
  CHECK(controller.set_room(1, {true, 20, 24}));
  writes = settle(bus, controller);
  CHECK(writes.size() == 1);
  CHECK(writes[0].kind == 5);
  CHECK(writes[0].data[0] == 1 && writes[0].data[1] == 1 && writes[0].data[2] == 1 && writes[0].data[4] == 1);
}

static void test_room_opens_before_it_becomes_reference() {
  Bus bus;
  ready(bus, 2);
  set_open(bus, 2, false);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::LOW);
  CHECK(controller.set_room(1, {false, 20, 24}));
  CHECK(controller.set_room(2, {false, 20, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[0] == 0);
  CHECK(controller.set_room(2, {true, 20, 22}));
  const auto writes = settle(bus, controller);
  CHECK(writes.size() == 3);
  CHECK(writes[0].kind == 3 && writes[0].data[0] == 2 && (writes[0].data[1] & 0x80) && writes[0].data[3] == 44);
  CHECK(writes[1].kind == 5 && writes[1].data[0] == 1 && writes[1].data[4] == 2);
  CHECK(writes[2].kind == 3 && writes[2].data[0] == 1 && !(writes[2].data[1] & 0x80));
}

static void test_reference_moves_before_its_zone_closes() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::LOW);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  CHECK(controller.set_room(1, {false, 20, 24}));
  const auto writes = settle(bus, controller);
  CHECK(writes.size() == 2);
  CHECK(writes[0].kind == 5 && writes[0].data[4] == 2);
  CHECK(writes[1].kind == 3 && writes[1].data[0] == 1 && !(writes[1].data[1] & 0x80));
}

static void test_last_room_off_stops_the_ac() {
  Bus bus;
  ready(bus, 1);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(1, {true, 20, 24}));
  settle(bus, controller);
  CHECK(controller.set_room(1, {false, 20, 24}));
  const auto writes = settle(bus, controller);
  CHECK(writes.size() == 1);
  CHECK(writes[0].kind == 5 && writes[0].data[0] == 0 && writes[0].data[4] == 1);
  CHECK(bus.session.bank().find(3, 1)->data[1] & 0x80);
  CHECK(controller.action(1) == Action::OFF);
}

static void test_setpoints_follow_mode_and_offset() {
  Bus bus;
  ready(bus, 1);
  Controller controller(bus.session);
  controller.set_fan(Fan::MEDIUM);
  CHECK(controller.set_room(1, {true, 19, 23}));
  controller.set_mode(Mode::COOL);
  // The AC's own setpoint follows the reference room's, for ACs that regulate on return air.
  auto writes = settle(bus, controller);
  CHECK(writes.size() == 2 && writes[0].kind == 3 && writes[0].data[3] == 46);
  CHECK(writes[1].kind == 5 && writes[1].data[3] == 46);
  controller.set_mode(Mode::HEAT);
  writes = settle(bus, controller);
  CHECK(writes.size() == 2);
  CHECK(writes[0].kind == 3 && writes[0].data[3] == 38);
  CHECK(writes[1].kind == 5 && writes[1].data[0] == 1 && writes[1].data[1] == 2 && writes[1].data[3] == 38);
  CHECK(controller.set_offset(1, 0.6f));
  CHECK(std::abs(controller.temperature(1) - 25.1f) < 0.001f);
  writes = settle(bus, controller);
  CHECK(writes.size() == 2 && writes[0].data[3] == 36 && writes[1].data[3] == 36);
  controller.set_mode(Mode::FAN);
  writes = settle(bus, controller);
  CHECK(writes.size() == 1 && writes[0].kind == 5 && writes[0].data[0] == 1 && writes[0].data[1] == 3);
  CHECK(!controller.set_room(1, {true, 25, 23}));
  CHECK(!controller.set_room(1, {true, 15, 23}));
  CHECK(!controller.set_room(1, {true, 20, NAN}));
  CHECK(!controller.set_room(11, {true, 20, 24}));
  CHECK(controller.set_room(1, {true, 20.4f, 23.6f}));
  CHECK(controller.room(1)->heat_to == 20 && controller.room(1)->cool_to == 24);
  CHECK(!controller.set_offset(1, 6));
}

static void test_idle_changeover_stays_idle() {
  for (const auto initial : {Mode::COOL, Mode::HEAT}) {
    for (const bool reverse : {false, true}) {
      Bus bus;
      ready(bus, 3);
      Controller controller(bus.session);
      controller.set_mode(initial);
      controller.set_fan(Fan::LOW);
      for (uint8_t zone = 1; zone <= 3; zone++) {
        CHECK(controller.set_room(zone, {true, 20, 24}));
        set_temperature(bus, zone, 22);
      }
      settle(bus, controller);
      const auto mode = initial == Mode::COOL ? Mode::HEAT : Mode::COOL;
      controller.set_mode(mode);
      controller.step(bus.now);
      auto writes = next_writes(bus);
      CHECK(writes.size() == 4);
      CHECK(std::count_if(writes.begin(), writes.end(), [](const Record &record) {
              return record.kind == kind::SYSTEM && record.data[field::POWER] == 1;
            }) == 1);
      if (reverse) std::reverse(writes.begin(), writes.end());
      for (size_t i = 0; i <= writes.size(); i++) {
        if (i > 0) apply(bus, writes[i - 1]);
        controller.step(bus.now);
        CHECK(controller.mode() == mode);
        CHECK(controller.ac_action() == Action::IDLE);
        CHECK(controller.ac_mode() == (i == writes.size() ? mode : initial));
        CHECK(controller.ac_fan() == Fan::LOW);
        for (uint8_t zone = 1; zone <= 3; zone++) {
          CHECK(controller.action(zone) == Action::IDLE);
          CHECK(controller.room(zone)->on);
        }
      }
      CHECK(!bus.session.writes_pending());
    }
  }
}

static void test_off_during_changeover_does_not_restart() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  for (uint8_t zone : {1, 2}) CHECK(controller.set_room(zone, {true, 20, 24}));
  settle(bus, controller);
  controller.set_mode(Mode::HEAT);
  controller.step(bus.now);
  const auto writes = next_writes(bus);
  CHECK(writes.size() == 3);
  controller.set_mode(Mode::OFF);
  for (const auto &write : writes) apply(bus, write);
  for (const auto &write : settle(bus, controller)) {
    CHECK(write.kind != kind::SYSTEM || write.data[field::POWER] == 0);
  }
  CHECK(controller.ac_mode() == Mode::OFF);
  CHECK(controller.ac_action() == Action::OFF);
}

static void test_changeover_with_demand_becomes_active() {
  for (const bool heating : {false, true}) {
    Bus bus;
    ready(bus, 1);
    Controller controller(bus.session);
    controller.set_mode(heating ? Mode::COOL : Mode::HEAT);
    controller.set_room(1, {true, 20, 24});
    set_temperature(bus, 1, heating ? 18 : 26);
    settle(bus, controller);
    CHECK(controller.ac_action() == Action::IDLE);
    controller.set_mode(heating ? Mode::HEAT : Mode::COOL);
    controller.step(bus.now);
    const auto writes = next_writes(bus);
    CHECK(writes.size() == 2);
    apply(bus, writes[0]);
    CHECK(controller.ac_action() == Action::IDLE);
    apply(bus, writes[1]);
    CHECK(controller.ac_action() == (heating ? Action::HEATING : Action::COOLING));
    CHECK(controller.action(1) == controller.ac_action());
  }
}

static void test_batch_readback_requires_all_latest_records() {
  Bus bus;
  ready(bus, 3);
  Change target{kind::ZONE, 1};
  target.set(field::SETPOINT, 40);
  Change mode{kind::SYSTEM};
  mode.set(field::MODE, 2);
  bus.session.queue(target);
  bus.session.queue(mode);
  const auto writes = next_writes(bus);
  CHECK(writes.size() == 2);
  // Transport rejection retries the entire batch, byte for byte.
  bus.receive("getCAN 0 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == set_can(writes));
  apply(bus, writes[0]);
  CHECK(bus.session.bank().find(kind::ZONE, 1)->data[field::SETPOINT] == 48);
  // An unrelated temperature remains live while the command is being confirmed.
  set_temperature(bus, 3, 21);
  CHECK(bus.session.bank().find(kind::ZONE, 3)->data[field::WHOLE_DEGREES] == 21);
  // A later stale echo invalidates the earlier match; a transport ACK is not a readback.
  report(bus, *bus.session.bank().find(kind::ZONE, 1));
  apply(bus, writes[1]);
  bus.receive("getCAN 1 ");
  CHECK(bus.session.writes_pending());
  CHECK(bus.session.bank().find(kind::SYSTEM)->data[field::MODE] == 1);
  apply(bus, writes[0]);
  CHECK(!bus.session.writes_pending());
  CHECK(bus.session.bank().find(kind::ZONE, 1)->data[field::SETPOINT] == 40);
  CHECK(bus.session.bank().find(kind::SYSTEM)->data[field::MODE] == 2);
}

static void test_incomplete_batch_resynchronizes() {
  Bus bus;
  ready(bus, 2);
  Change target{kind::ZONE, 1};
  target.set(field::SETPOINT, 40);
  Change mode{kind::SYSTEM};
  mode.set(field::MODE, 2);
  bus.session.queue(target);
  bus.session.queue(mode);
  const auto writes = next_writes(bus);
  apply(bus, writes[0]);
  CHECK(bus.session.writes_pending());
  bus.now += 1000;
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN 0701000000600000000000000");
  bus.now += 4000;
  bus.session.tick(bus.now);
  CHECK(!bus.session.ready());
  CHECK(!bus.session.writes_pending());
  CHECK(bus.session.write_failures() == 1);
  CHECK(bus.session.discarded_writes() == "unconfirmed AC settings " + encode_record(writes[1]));
  ready(bus, 2);
  CHECK(bus.session.bank().find(kind::ZONE, 1)->data[field::SETPOINT] == 48);
  CHECK(bus.session.bank().find(kind::SYSTEM)->data[field::MODE] == 1);
}

static void test_target_sets_every_room() {
  Bus bus;
  ready(bus, 3);
  Controller controller(bus.session);
  controller.set_fan(Fan::LOW);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {false, 23, 25}));
  // Off has no edge to set.
  CHECK(!controller.set_target(22));
  controller.set_mode(Mode::COOL);
  settle(bus, controller);
  CHECK(controller.target() == 24);
  // Cooling, every room's cool-to follows, on or off, and a heat-to closer than 2 °C comes down with it. Zone 3 has no
  // room.
  CHECK(controller.set_target(21.6f));
  CHECK(controller.room(1)->heat_to == 20 && controller.room(1)->cool_to == 22);
  CHECK(controller.room(2)->heat_to == 20 && controller.room(2)->cool_to == 22);
  CHECK(!controller.room(3) && controller.ac_setpoint() == 22 && controller.target() == 22);
  settle(bus, controller);
  CHECK(bus.session.bank().find(3, 1)->data[3] == 44 && bus.session.bank().find(5)->data[3] == 44);
  // At the bottom of the range, cool-to stops 2 °C short.
  CHECK(controller.set_target(16));
  CHECK(controller.room(1)->heat_to == 16 && controller.room(1)->cool_to == 18 && controller.ac_setpoint() == 16);
  // Heating, heat-to follows and pushes cool-to up.
  controller.set_mode(Mode::HEAT);
  settle(bus, controller);
  CHECK(controller.target() == 16);
  CHECK(controller.set_target(23));
  CHECK(controller.room(1)->heat_to == 23 && controller.room(1)->cool_to == 25 && controller.target() == 23);
  CHECK(!controller.set_target(15) && !controller.set_target(33));
  CHECK(!controller.set_target(NAN));
  // Auto accepts explicit edges, not a single target whose meaning depends on its side.
  set_temperature(bus, 1, 17.0f);
  controller.set_mode(Mode::AUTO);
  CHECK(!controller.set_target(21));
  CHECK(controller.set_targets(21, 25));
  CHECK(controller.target_room()->heat_to == 21 && controller.target_room()->cool_to == 25);
  controller.set_mode(Mode::FAN);
  CHECK(!controller.set_target(22));
  // At the top of the range, heat-to stops 2 °C short so the range keeps its width; the AC's own setpoint takes it all.
  controller.set_mode(Mode::HEAT);
  CHECK(controller.set_target(32));
  CHECK(controller.room(1)->heat_to == 30 && controller.room(1)->cool_to == 32 && controller.ac_setpoint() == 32);
}

static void test_system_range_and_room_edits_are_independent() {
  Bus bus;
  Controller controller(bus.session);
  controller.set_mode(Mode::AUTO);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {false, 19, 25}));
  CHECK(controller.set_airflow(3, {true, 50}));
  // Targets are owned locally and can be set before the control box connects.
  CHECK(controller.set_targets(18, 26));
  for (uint8_t zone : {1, 2}) {
    CHECK(controller.room(zone)->heat_to == 18 && controller.room(zone)->cool_to == 26);
  }
  CHECK(controller.room(1)->on && !controller.room(2)->on);
  CHECK(controller.airflow(3)->on && controller.airflow(3)->position == 50);
  CHECK(!controller.room(3));
  CHECK(controller.target_room()->heat_to == 18 && controller.target_room()->cool_to == 26);
  ready(bus, 3);
  settle(bus, controller);
  CHECK(controller.set_room(1, {true, 19, 27}));
  settle(bus, controller);
  CHECK(controller.target_room()->heat_to == 19 && controller.target_room()->cool_to == 27);
  CHECK(controller.room(2)->heat_to == 18 && controller.room(2)->cool_to == 26);
  // Neither mode changes nor an old bus setpoint can reapply the previous whole-house override.
  controller.set_mode(Mode::HEAT);
  settle(bus, controller);
  controller.set_mode(Mode::AUTO);
  auto stale = *bus.session.bank().find(kind::ZONE, 1);
  stale.data[field::SETPOINT] = 36;
  report(bus, stale);
  settle(bus, controller);
  CHECK(controller.room(1)->heat_to == 19 && controller.room(1)->cool_to == 27);
  CHECK(controller.room(2)->heat_to == 18 && controller.room(2)->cool_to == 26);
  CHECK(controller.set_targets({}, 25));
  CHECK(controller.room(1)->heat_to == 19 && controller.room(1)->cool_to == 25);
  CHECK(controller.room(2)->heat_to == 18 && controller.room(2)->cool_to == 25);
  CHECK(controller.set_targets(20, {}));
  for (uint8_t zone : {1, 2}) CHECK(controller.room(zone)->heat_to == 20 && controller.room(zone)->cool_to == 25);
}

static void test_system_range_validation_is_atomic() {
  Bus bus;
  Controller controller(bus.session);
  CHECK(controller.set_room(1, {true, 20, 25}));
  CHECK(controller.set_room(2, {false, 18, 24}));
  CHECK(!controller.set_targets({}, {}));
  CHECK(!controller.set_targets(NAN, 24));
  CHECK(!controller.set_targets(20, INFINITY));
  CHECK(!controller.set_targets(15, 24));
  CHECK(!controller.set_targets(20, 33));
  CHECK(!controller.set_targets(25, 20));
  CHECK(controller.room(1)->heat_to == 20 && controller.room(1)->cool_to == 25);
  CHECK(controller.room(2)->heat_to == 18 && controller.room(2)->cool_to == 24);
  // Normalize the range once, so rooms with different old ranges get the same pair.
  CHECK(controller.set_targets(20, 21));
  for (uint8_t zone : {1, 2}) CHECK(controller.room(zone)->heat_to == 19 && controller.room(zone)->cool_to == 21);
  CHECK(controller.set_targets(32, {}));
  for (uint8_t zone : {1, 2}) CHECK(controller.room(zone)->heat_to == 30 && controller.room(zone)->cool_to == 32);
  CHECK(controller.set_targets({}, 16));
  for (uint8_t zone : {1, 2}) CHECK(controller.room(zone)->heat_to == 16 && controller.room(zone)->cool_to == 18);
}

static void test_sensorless_system_keeps_its_single_target() {
  Bus bus;
  Controller controller(bus.session);
  controller.set_airflow(1, {true, 50});
  controller.set_mode(Mode::AUTO);
  CHECK(!controller.target_room());
  CHECK(!controller.set_targets(20, 24));
  CHECK(controller.set_target(22));
  CHECK(controller.target() == 22);
}

static void test_ranges_keep_two_degrees() {
  Bus bus;
  ready(bus, 1);
  Controller controller(bus.session);
  CHECK(controller.set_room(1, {true, 20, 24}));
  // Heat-to set within 2 °C of cool-to pushes cool-to up; cool-to set within 2 °C pushes heat-to down.
  CHECK(controller.set_room(1, {true, 23, 24}));
  CHECK(controller.room(1)->heat_to == 23 && controller.room(1)->cool_to == 25);
  CHECK(controller.set_room(1, {true, 23, 24}));
  CHECK(controller.room(1)->heat_to == 22 && controller.room(1)->cool_to == 24);
  // Against a limit, the edge that was set gives way.
  CHECK(controller.set_room(1, {true, 32, 32}));
  CHECK(controller.room(1)->heat_to == 30 && controller.room(1)->cool_to == 32);
  CHECK(controller.set_room(1, {true, 30, 17}) == false);
  CHECK(controller.set_room(1, {true, 16, 20}) && controller.set_room(1, {true, 16, 17}));
  CHECK(controller.room(1)->heat_to == 16 && controller.room(1)->cool_to == 18);
}

static void test_reference_moves_once_satisfied() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::MEDIUM);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  // A reference past its edge keeps the AC working and stays, however far another room is.
  set_temperature(bus, 2, 26.0f);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  // Once satisfied, it stays until another room calls.
  set_temperature(bus, 1, 23.6f);
  set_temperature(bus, 2, 24.3f);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  set_temperature(bus, 2, 24.4f);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 2);
  // The next move waits a hold.
  set_temperature(bus, 1, 25.0f);
  set_temperature(bus, 2, 23.8f);
  wait(bus, controller, controller.tuning.reference_hold - 2000);
  CHECK(!bus.session.writes_pending());
  wait(bus, controller, 3000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
}

static void test_trims_open_dampers_early() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::MEDIUM);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 24}));
  set_temperature(bus, 1, 25.0f);
  settle(bus, controller);
  // A room past CALL gets its setpoint a degree past its edge, at most once per hold; the reference never does.
  wait(bus, controller, controller.tuning.trim_hold + 1000);
  const auto writes = settle(bus, controller);
  CHECK(writes.size() == 1 && writes[0].kind == 3 && writes[0].data[0] == 2 && writes[0].data[3] == 46);
  CHECK(bus.session.bank().find(3, 1)->data[3] == 48);
  // Inside its edge by less than the release it keeps the trim; further in, it comes off.
  set_temperature(bus, 2, 23.9f);
  wait(bus, controller, controller.tuning.trim_hold + 1000);
  CHECK(!bus.session.writes_pending());
  set_temperature(bus, 2, 23.7f);
  wait(bus, controller, 2000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(3, 2)->data[3] == 48);
  // Heating trims the other way.
  controller.set_mode(Mode::HEAT);
  set_temperature(bus, 1, 19.0f);
  set_temperature(bus, 2, 19.5f);
  settle(bus, controller);
  wait(bus, controller, controller.tuning.trim_hold + 1000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  CHECK(bus.session.bank().find(3, 1)->data[3] == 40 && bus.session.bank().find(3, 2)->data[3] == 42);
}

static void test_small_rooms_never_run_the_ac() {
  Bus bus;
  ready(bus, 2);
  auto limits = *bus.session.bank().find(4, 2);
  limits.data[2] = 15;
  report(bus, limits);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 24}));
  set_temperature(bus, 2, 28.0f);
  set_open(bus, 1, false);
  auto system = *bus.session.bank().find(5);
  system.data[4] = 2;
  report(bus, system);
  // At 15 % of the other zone's airflow, zone 2 can't carry the AC: a larger room takes the reference at once,
  // whatever their demand.
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  // On its own it stays open but doesn't run the AC, and the reference stays put.
  CHECK(controller.set_room(1, {false, 20, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[0] == 0 && bus.session.bank().find(5)->data[4] == 1);
  CHECK(bus.session.bank().find(3, 2)->data[1] & 0x80);
  // Three times the airflow carries it.
  CHECK(controller.set_airflow_weight(2, 3));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[0] == 1 && bus.session.bank().find(5)->data[4] == 2);
}

static void test_small_rooms_leave_autos_side_alone() {
  Bus bus;
  ready(bus, 2);
  auto limits = *bus.session.bank().find(4, 2);
  limits.data[2] = 15;
  report(bus, limits);
  Controller controller(bus.session);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 24}));
  set_temperature(bus, 1, 19.0f);
  controller.set_mode(Mode::AUTO);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[1] == 2);
  // Warmed past its cool-to by leakage, the small zone doesn't turn the house to cooling.
  set_temperature(bus, 1, 20.4f);
  set_temperature(bus, 2, 27.0f);
  wait(bus, controller, controller.tuning.changeover_hold + 60000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[1] == 2);
}

static void test_auto_changeover() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::AUTO);
  controller.set_fan(Fan::LOW);
  CHECK(controller.set_room(1, {true, 21, 24}));
  CHECK(controller.set_room(2, {true, 21, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[1] == 1);
  // Rooms inside their ranges never change the side.
  set_temperature(bus, 1, 22.0f);
  set_temperature(bus, 2, 22.0f);
  wait(bus, controller, controller.tuning.changeover_hold + 5000);
  CHECK(!bus.session.writes_pending());
  // A cold room waits while another room still calls for cooling.
  set_temperature(bus, 1, 25.0f);
  set_temperature(bus, 2, 19.5f);
  wait(bus, controller, controller.tuning.changeover_hold + 5000);
  CHECK(!bus.session.writes_pending());
  set_temperature(bus, 1, 24.0f);
  wait(bus, controller, controller.tuning.changeover_hold - 2000);
  CHECK(!bus.session.writes_pending());
  wait(bus, controller, 3000);
  const auto writes = settle(bus, controller);
  CHECK(writes.size() == 3);
  CHECK(writes[0].kind == 3 && writes[0].data[3] == 42 && writes[1].kind == 3 && writes[1].data[3] == 42);
  CHECK(writes[2].kind == 5 && writes[2].data[0] == 1 && writes[2].data[1] == 2 && writes[2].data[4] == 2);
}

static void test_auto_weighs_rooms_by_airflow() {
  Bus bus;
  ready(bus, 2);
  auto system = *bus.session.bank().find(5);
  system.data[0] = 0;
  report(bus, system);
  Controller controller(bus.session);
  controller.set_fan(Fan::LOW);
  // A large room 1 °C warm outweighs a standard room 2 °C cold.
  CHECK(controller.set_airflow_weight(1, 3) && controller.set_airflow_weight(2, 1));
  CHECK(controller.set_room(1, {true, 21, 24}));
  CHECK(controller.set_room(2, {true, 21, 24}));
  set_temperature(bus, 1, 25.0f);
  set_temperature(bus, 2, 19.0f);
  controller.set_mode(Mode::AUTO);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[0] == 1 && bus.session.bank().find(5)->data[1] == 1);
}

static void test_auto_starts_on_the_side_rooms_want() {
  Bus bus;
  ready(bus, 1);
  auto system = *bus.session.bank().find(5);
  system.data[1] = 2;
  report(bus, system);
  Controller controller(bus.session);
  CHECK(controller.set_room(1, {true, 20, 23}));
  controller.set_mode(Mode::AUTO);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[1] == 1);
  CHECK(bus.session.bank().find(3, 1)->data[3] == 46);
}

static void test_auto_fan_follows_airflow() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  // Zone 2 has no room: it counts toward the system's airflow but never asks for air.
  CHECK(controller.set_airflow_weight(1, 3) && controller.set_airflow_weight(2, 1));
  CHECK(controller.set_room(1, {true, 20, 24}));
  set_temperature(bus, 1, 25.0f);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
  // Three quarters of the airflow asking fully is high and a third of that is medium. The fan steps down a speed per
  // two holds.
  set_temperature(bus, 1, 24.3f);
  wait(bus, controller, 2 * controller.tuning.fan_hold - 2000);
  CHECK(!bus.session.writes_pending());
  wait(bus, controller, 3000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 2);
  set_temperature(bus, 1, 23.5f);
  wait(bus, controller, 2 * controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 1);
  // It steps up a speed per hold.
  set_temperature(bus, 1, 25.0f);
  wait(bus, controller, controller.tuning.fan_hold - 2000);
  CHECK(!bus.session.writes_pending());
  wait(bus, controller, 3000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 2);
  wait(bus, controller, controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
  // The same demand from a smaller zone asks for less, and a room past its full damper asks for more.
  CHECK(controller.set_airflow_weight(1, 0.4f));
  wait(bus, controller, 2 * controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 2);
  set_temperature(bus, 1, 26.0f);
  wait(bus, controller, controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
  controller.set_fan(Fan::HIGH);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
}

static void test_auto_fan_serves_a_room_behind() {
  Bus bus;
  ready(bus, 3);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  // Zones 2 and 3 have no room; zone 1 is a tenth of the system's airflow, so its share alone stays low.
  CHECK(controller.set_airflow_weight(1, 0.2f) && controller.set_airflow_weight(2, 1) &&
        controller.set_airflow_weight(3, 1));
  CHECK(controller.set_room(1, {true, 20, 24}));
  set_temperature(bus, 1, 24.3f);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 1);
  set_temperature(bus, 1, 25.3f);
  wait(bus, controller, controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 2);
  set_temperature(bus, 1, 26.3f);
  wait(bus, controller, controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
}

static void set_damper(Bus &bus, uint8_t zone, uint8_t position) {
  auto state = *bus.session.bank().find(3, zone);
  state.data[1] = (state.data[1] & 0x80) | position;
  report(bus, state);
}

static void test_auto_fan_is_capped_by_open_dampers() {
  Bus bus;
  ready(bus, 4);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(1, {true, 20, 24}));
  for (uint8_t zone : {2, 3, 4}) CHECK(controller.set_room(zone, {false, 20, 24}));
  set_temperature(bus, 1, 26.3f);
  set_damper(bus, 1, 100);
  // A room 2 °C past its edge asks for high, but a quarter of the system's airflow open only takes medium, judged on
  // the dampers as the batch that closes the other zones leaves them.
  for (const auto &write : settle(bus, controller)) CHECK(write.kind != 5 || write.data[2] == 2);
  CHECK(bus.session.bank().find(5)->data[2] == 2);
  // A second room calling opens enough for high, after the hold.
  CHECK(controller.set_room(2, {true, 20, 24}));
  set_temperature(bus, 2, 26.3f);
  settle(bus, controller);
  set_damper(bus, 2, 100);
  wait(bus, controller, controller.tuning.fan_hold + 5000);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
  // High holds with three quarters of its share open, and drops at once below that.
  set_damper(bus, 2, 30);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
  CHECK(controller.set_room(2, {false, 20, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 2);
  // Under a tenth of the airflow open is low, however far the room is past its edge.
  CHECK(controller.set_airflow_weight(1, 0.2f));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 1);
  // A fan speed set by hand stands.
  controller.set_fan(Fan::HIGH);
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[2] == 3);
}

static void test_small_is_relative_to_the_largest_zone() {
  Bus bus;
  ready(bus, 3);
  auto limits = *bus.session.bank().find(4, 2);
  limits.data[2] = 60;
  report(bus, limits);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::LOW);
  // An 8" bedroom at 60 % next to 12" rooms: a quarter of their airflow, but no throttled zone.
  CHECK(controller.set_airflow_weight(1, 1) && controller.set_airflow_weight(2, 0.44f) &&
        controller.set_airflow_weight(3, 1));
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 24}));
  CHECK(controller.set_room(3, {true, 20, 24}));
  set_temperature(bus, 2, 27.0f);
  settle(bus, controller);
  CHECK(controller.set_room(1, {false, 20, 24}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[4] == 2);
}

static void test_airflow_weights_stand_alone() {
  Bus bus;
  ready(bus, 3);
  Controller controller(bus.session);
  CHECK(!controller.set_airflow_weight(2, 0) && !controller.set_airflow_weight(2, NAN) &&
        !controller.set_airflow_weight(11, 1));
  // Setting one zone's weight leaves the others at 1.
  CHECK(controller.set_airflow_weight(1, 3));
  CHECK(controller.airflow_weight(1) == 3 && controller.airflow_weight(2) == 1 && controller.airflow_weight(3) == 1);
}

static void set_sensorless(Bus &bus, uint8_t zone) {
  auto state = *bus.session.bank().find(3, zone);
  state.data[2] = 0;
  report(bus, state);
}

static void test_airflow_zones_hold_their_position() {
  Bus bus;
  ready(bus, 2);
  set_sensorless(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::LOW);
  CHECK(!controller.set_airflow(2, {true, 0}) && !controller.set_airflow(2, {true, 42}));
  CHECK(!controller.set_airflow(2, {true, 105}) && !controller.set_airflow(11, {true, 40}));
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_airflow(2, {true, 40}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(3, 2)->data[1] == (0x80 | 40));
  CHECK(bus.session.bank().find(5)->data[4] == 1);
  CHECK(controller.set_airflow(2, {false, 40}));
  settle(bus, controller);
  CHECK(!(bus.session.bank().find(3, 2)->data[1] & 0x80));
  // With no temperature room on, the AC runs on its own setpoint and return air: reference 0, and Auto is
  // the AC's own.
  CHECK(controller.set_ac_setpoint(22.4f) && controller.ac_setpoint() == 22);
  CHECK(!controller.set_ac_setpoint(40));
  CHECK(controller.set_room(1, {false, 20, 24}));
  CHECK(controller.set_airflow(2, {true, 60}));
  controller.set_mode(Mode::AUTO);
  settle(bus, controller);
  const auto &system = bus.session.bank().find(5)->data;
  CHECK(system[0] == 1 && system[1] == 4 && system[3] == 44 && system[4] == 0);
  CHECK(controller.target() == 22);
  CHECK(!(bus.session.bank().find(3, 1)->data[1] & 0x80));
  CHECK(bus.session.bank().find(3, 2)->data[1] == (0x80 | 60));
  CHECK(controller.ac_action() == Action::IDLE);
  controller.set_mode(Mode::COOL);
  settle(bus, controller);
  CHECK(controller.ac_action() == Action::COOLING);
  // The last zone off stops the AC.
  CHECK(controller.set_airflow(2, {false, 60}));
  settle(bus, controller);
  CHECK(bus.session.bank().find(5)->data[0] == 0);
}

static void test_shutdown_preserves_air_paths() {
  for (const bool sensor_reference : {false, true}) {
    for (const bool system_off : {false, true}) {
      Bus bus;
      ready(bus, 3);
      Controller controller(bus.session);
      controller.set_mode(Mode::COOL);
      controller.set_fan(Fan::HIGH);
      auto config = *bus.session.bank().find(kind::CONFIG);
      config.data[field::CONSTANT_COUNT] = 1;
      config.data[field::FIRST_CONSTANT] = 3;
      report(bus, config);
      if (sensor_reference) {
        CHECK(controller.set_room(1, {true, 20, 24}));
      } else {
        set_sensorless(bus, 1);
        CHECK(controller.set_airflow(1, {true, 20}));
      }
      for (uint8_t zone : {2, 3}) set_sensorless(bus, zone);
      CHECK(controller.set_airflow(2, {true, 20}));
      CHECK(controller.set_airflow(3, {false, 100}));
      settle(bus, controller);
      const auto system = *bus.session.bank().find(kind::SYSTEM);
      CHECK(system.data[field::REFERENCE] == (sensor_reference ? 1 : 0));
      std::array<Record, 3> zones;
      for (uint8_t zone = 1; zone <= 3; zone++) {
        zones[zone - 1] = *bus.session.bank().find(kind::ZONE, zone);
        CHECK(zones[zone - 1].data[field::DAMPER] & field::OPEN);
      }
      if (system_off) {
        controller.set_mode(Mode::OFF);
      } else {
        if (sensor_reference)
          CHECK(controller.set_room(1, {false, 20, 24}));
        else
          CHECK(controller.set_airflow(1, {false, 20}));
        CHECK(controller.set_airflow(2, {false, 20}));
      }
      controller.step(bus.now);
      const auto stop = next_write(bus);
      CHECK(stop.kind == kind::SYSTEM && stop.data[field::POWER] == 0);
      CHECK(stop.data[field::REFERENCE] == system.data[field::REFERENCE]);
      bus.receive("getCAN 1 ");
      controller.step(bus.now);
      CHECK(bus.session.writes_pending());
      CHECK(bus.ping() == "ackCAN 1");
      CHECK(bus.ping() == "setCAN ");
      apply(bus, stop);
      // A reported power-off is not fan-stop feedback. Do not close paths after it, or on an elapsed timer.
      CHECK(settle(bus, controller).empty());
      wait(bus, controller, 5 * 60000);
      CHECK(settle(bus, controller).empty());
      for (uint8_t zone = 1; zone <= 3; zone++) {
        CHECK(bus.session.bank().find(kind::ZONE, zone)->data == zones[zone - 1].data);
      }
      // A later start still applies the saved zone requests and resumes regulation.
      if (sensor_reference)
        CHECK(controller.set_room(1, {true, 20, 24}));
      else
        CHECK(controller.set_airflow(1, {true, 20}));
      controller.set_mode(Mode::COOL);
      settle(bus, controller);
      CHECK(controller.ac_mode() == Mode::COOL);
      CHECK(bus.session.bank().find(kind::ZONE, 1)->data[field::DAMPER] & field::OPEN);
    }
  }
}

static void test_constant_zones_hold_up_airflow() {
  Bus bus;
  ready(bus, 2);
  set_sensorless(bus, 2);
  auto config = *bus.session.bank().find(1);
  config.data[2] = 1;
  config.data[3] = 2;
  report(bus, config);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  controller.set_fan(Fan::LOW);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_airflow(2, {false, 100}));
  // Zone 1 is open at 50 %, half of one constant's 100 %: the constant zone opens to make up the rest.
  settle(bus, controller);
  CHECK(bus.session.bank().find(3, 2)->data[1] == (0x80 | 50));
  auto state = *bus.session.bank().find(3, 1);
  state.data[1] = 0x80 | 100;
  report(bus, state);
  settle(bus, controller);
  CHECK(!(bus.session.bank().find(3, 2)->data[1] & 0x80));
}

static void test_resync_reapplies_rooms() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {false, 20, 24}));
  settle(bus, controller);
  CHECK(!(bus.session.bank().find(3, 2)->data[1] & 0x80));
  bus.now += 5000;
  bus.session.tick(bus.now);
  CHECK(!bus.session.ready());
  controller.step(bus.now);
  ready(bus, 2);
  settle(bus, controller);
  CHECK(!(bus.session.bank().find(3, 2)->data[1] & 0x80));
}

static void test_three_constant_zones_fill_the_shortfall() {
  Bus bus;
  ready(bus, 4);
  auto config = *bus.session.bank().find(kind::CONFIG);
  config.data[field::CONSTANT_COUNT] = 3;
  for (uint8_t i = 0; i < 3; i++) config.data[field::FIRST_CONSTANT + i] = i + 2;
  report(bus, config);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  for (uint8_t zone = 1; zone <= 4; zone++) {
    set_sensorless(bus, zone);
    CHECK(controller.set_airflow(zone, {zone == 1, 5}));
  }
  settle(bus, controller);
  CHECK(controller.damper(1) == 5);
  // A 295 % shortfall needs all three constants fully open (rounded up to 5 % steps).
  for (uint8_t zone = 2; zone <= 4; zone++) CHECK(controller.damper(zone) == 100);
  CHECK(settle(bus, controller).empty());
}

static void test_actions() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(1, {true, 20, 24}));
  CHECK(controller.set_room(2, {true, 20, 25}));
  settle(bus, controller);
  CHECK(controller.action(1) == Action::COOLING);
  CHECK(controller.action(2) == Action::IDLE);
  CHECK(controller.reference() == 1 && controller.setpoint(1) == 24);
  CHECK(controller.ac_action() == Action::COOLING);
  set_temperature(bus, 1, 23.8f);
  CHECK(controller.ac_action() == Action::IDLE);
  controller.set_mode(Mode::DRY);
  settle(bus, controller);
  CHECK(controller.action(1) == Action::DRYING);
  controller.set_mode(Mode::OFF);
  settle(bus, controller);
  CHECK(controller.action(1) == Action::OFF);
}

static void test_reference_requires_a_readable_temperature() {
  Bus bus;
  ready(bus, 2);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  for (uint8_t zone : {1, 2}) CHECK(controller.set_room(zone, {true, 20, 24}));
  settle(bus, controller);
  CHECK(controller.reference() == 1);
  for (uint8_t zone : {1, 2}) {
    auto missing = *bus.session.bank().find(kind::ZONE, zone);
    missing.data[field::WHOLE_DEGREES] = 0xFF;
    report(bus, missing);
    settle(bus, controller);
    if (zone == 1) {
      CHECK(controller.reference() == 2 && controller.ac_mode() == Mode::COOL);
    } else {
      CHECK(controller.ac_mode() == Mode::OFF);
    }
  }
  set_temperature(bus, 1, 25);
  settle(bus, controller);
  CHECK(controller.reference() == 1 && controller.ac_mode() == Mode::COOL);
}

static void test_probe_dropout_keeps_its_cached_temperature_bytes() {
  Bus bus;
  ready(bus);
  Controller controller(bus.session);
  controller.set_mode(Mode::COOL);
  CHECK(controller.set_room(3, {true, 20, 24}));
  // Attic before and after its probe battery was removed. Only the sensor type changed in the zone record.
  bus.receive("getCAN 1 0703abcde0303e40130170800 0703abcde040300410001003f");
  settle(bus, controller);
  CHECK(controller.reference() == 3 && controller.temperature(3) == 23.8f);
  bus.receive("getCAN 1 0703abcde0303e40030170800 0703abcde040300410001023f");
  settle(bus, controller);
  CHECK(!std::isfinite(controller.temperature(3)) && controller.ac_mode() == Mode::OFF);
  CHECK(controller.room(3)->on && bus.session.bank().find(kind::LIMITS, 3)->data[field::SENSOR_ERROR] == 2);
  bus.receive("getCAN 1 0703abcde0303a80130180200 0703abcde0403004100010033");
  settle(bus, controller);
  CHECK(controller.reference() == 3 && controller.ac_mode() == Mode::COOL && controller.temperature(3) == 24.2f);
}

void test_controls() {
  test_probe_dropout_keeps_its_cached_temperature_bytes();
  test_reference_requires_a_readable_temperature();
  test_three_constant_zones_fill_the_shortfall();
  test_idle_changeover_stays_idle();
  test_batch_readback_requires_all_latest_records();
  test_incomplete_batch_resynchronizes();
  test_off_during_changeover_does_not_restart();
  test_changeover_with_demand_becomes_active();
  test_balancing_readback();
  test_write_nack_and_crc_failure();
  test_confirmed_writes();
  test_merge_and_preserve();
  test_new_command_during_readback();
  test_wrong_readback_and_timeout();
  test_power_follows_rooms_and_unmanaged_zones();
  test_room_opens_before_it_becomes_reference();
  test_reference_moves_before_its_zone_closes();
  test_last_room_off_stops_the_ac();
  test_setpoints_follow_mode_and_offset();
  test_target_sets_every_room();
  test_ranges_keep_two_degrees();
  test_system_range_and_room_edits_are_independent();
  test_system_range_validation_is_atomic();
  test_sensorless_system_keeps_its_single_target();
  test_reference_moves_once_satisfied();
  test_trims_open_dampers_early();
  test_small_rooms_never_run_the_ac();
  test_small_rooms_leave_autos_side_alone();
  test_auto_changeover();
  test_auto_weighs_rooms_by_airflow();
  test_auto_starts_on_the_side_rooms_want();
  test_auto_fan_follows_airflow();
  test_auto_fan_serves_a_room_behind();
  test_auto_fan_is_capped_by_open_dampers();
  test_small_is_relative_to_the_largest_zone();
  test_airflow_zones_hold_their_position();
  test_shutdown_preserves_air_paths();
  test_airflow_weights_stand_alone();
  test_constant_zones_hold_up_airflow();
  test_resync_reapplies_rooms();
  test_actions();
}
