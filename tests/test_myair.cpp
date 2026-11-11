#include "test_support.h"

static void test_frames() {
  CHECK(frame("Ping") == "<U>Ping</U=db>");
  CHECK(frame("getSystemData") == "<U>getSystemData</U=15>");
  CHECK(frame("ackCAN 1") == "<U>ackCAN 1</U=aa>");
  CHECK(frame("ackCAN 0") == "<U>ackCAN 0</U=94>");
  CHECK(frame("setCAN ") == "<U>setCAN </U=b2>");
  CHECK(frame("setCAN 0701000000600000000000000") == "<U>setCAN 0701000000600000000000000</U=6a>");
  FrameParser parser;
  std::vector<Frame> frames;
  auto feed = [&](const std::string &text) {
    for (uint8_t byte : text) {
      auto result = parser.feed(byte);
      if (result) frames.push_back(*result);
    }
  };
  feed("noise<U>getCAN 1 truncated<U>Ping</U=DB> <U>getCAN 1 </U=xx>");
  CHECK(frames.size() == 2);
  CHECK(frames[0].valid && frames[0].payload == "Ping");
  CHECK(!frames[1].valid);
  feed("<U>" + std::string(3000, 'a') + "<U>Ping</U=db>");
  CHECK(frames.size() == 3 && frames.back().valid);
  feed(fixture());
  CHECK(frames.size() == 4 && frames.back().valid);
}

static void test_records() {
  const std::string raw = "0804ABCDE2e0123456789aBcD";
  auto record = parse_record(raw);
  CHECK(record.has_value());
  CHECK(record->unit_type == 8 && record->direction == 4 && record->unit_id == 0xabcde);
  CHECK(record->kind == 0x2E && record->data[6] == 0xCD);
  CHECK(encode_record(*record) == "0804abcde2e0123456789abcd");
  CHECK(!parse_record(raw + "0"));
  CHECK(!parse_record(raw.substr(1)));
  CHECK(!parse_record("0703abcde03zz000000000000"));
  bool accepted;
  std::vector<Record> records;
  CHECK(parse_get_can("getCAN 1 " + raw + " ", accepted, records));
  CHECK(accepted && records.size() == 1);
  CHECK(!parse_get_can("getCAN 1 " + raw + " invalid", accepted, records));
  CHECK(records.empty());
  CHECK(!parse_get_can("getCAN 10 ", accepted, records));
  CHECK(parse_get_can("getCAN 0 ", accepted, records));
  CHECK(!accepted && records.empty());

  RegisterBank bank;
  CHECK(bank.find(5) == nullptr && !bank.complete());
  auto announcement = request(8, 0xabcde, 0x0A);
  CHECK(!bank.apply(announcement));
  announcement.direction = 4;
  CHECK(bank.apply(announcement));
  CHECK(bank.apply(*record));
  CHECK(bank.find(0x2E)->data == record->data);
  record->unit_id = 0x11111;
  record->data[0] = 0xFF;
  CHECK(!bank.apply(*record));
  CHECK(bank.find(0x2E)->data[0] == 1);
  const auto ack = request(8, 0xabcde, 7);
  CHECK(encode_record(ack) == "0801abcde0700000000000000");
}

static void test_captured_snapshot() {
  Bus bus;
  start(bus);
  CHECK(bus.wire(fixture()).empty());
  CHECK(bus.session.ready());
  const auto &bank = bus.session.bank();
  CHECK(bank.unit_id() == 0xabcde && bank.unit_type() == 7);
  CHECK(bank.find(1)->data[1] == 10);
  CHECK(bank.find(5)->data == (std::array<uint8_t, 7>{1, 1, 1, 48, 1, 0, 0}));
  CHECK(bank.find(3, 1)->data == (std::array<uint8_t, 7>{1, 228, 1, 48, 25, 1, 0}));
  CHECK(bank.find(3, 10)->data[4] == 22 && bank.find(3, 10)->data[5] == 8);
  CHECK(bank.find(3, 11) == nullptr);
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN ");
  const auto revision = bus.session.revision();
  CHECK(bus.wire(fixture()).empty());
  CHECK(bus.session.revision() == revision);
}

static void test_sensor_report_counter() {
  Bus bus;
  bus.session.set_sensor(3, 0x01186e);
  start(bus);
  bus.receive(snapshot(3));
  CHECK(!bus.session.sensor_report(3));
  bus.receive("getCAN 1 0703abcde2b01186e63122a00");
  CHECK(bus.session.sensor_report(3) == 0x63);
  const auto revision = bus.session.revision();
  bus.receive("getCAN 1 0703abcde2b01186e63122a00 0703abcde2b01121078122a00 0703123452b01186e64122a00");
  bus.receive("getCAN 1 0701abcde2b01186e64122a00");
  CHECK(bus.session.revision() == revision);
  bus.receive(snapshot(3));
  CHECK(bus.session.sensor_report(3) == 0x63);
  bus.receive("getCAN 1 0703abcde2b01186eff122a00 0703abcde2b01186e00122a00");
  CHECK(bus.session.sensor_report(3) == 0);
  bus.session.set_sensor(1, 0x01186e);
  CHECK(!bus.session.sensor_report(1) && !bus.session.sensor_report(3));
  bus.receive("getCAN 1 0703abcde2b01186e01122a00");
  CHECK(bus.session.sensor_report(1) == 1 && !bus.session.sensor_report(3));
  bus.session.tick(bus.now + 5000);
  CHECK(!bus.session.sensor_report(1));
}

static void test_zone_counts_and_partial_snapshot() {
  for (uint8_t count : {0, 1, 3, 10}) {
    Bus bus;
    start(bus, 8);
    auto payload = snapshot(count, 8);
    // A snapshot may span several getCAN replies.
    const auto split = payload.find(' ', 9);
    bus.receive(payload.substr(0, split));
    CHECK(!bus.session.ready());
    CHECK(bus.ping() == "ackCAN 1");
    bus.receive("getCAN 1" + payload.substr(split));
    CHECK(bus.session.ready());
    CHECK(bus.session.bank().find(1)->data[1] == count);
    CHECK(bus.session.bank().find(3, count + 1) == nullptr);
  }
}

static void test_errors_do_not_change_state() {
  Bus bus;
  start(bus);
  bus.receive(snapshot(1));
  CHECK(bus.ping() == "ackCAN 1");
  auto original = bus.session.bank().find(5)->data;
  auto changed = *bus.session.bank().find(5);
  changed.data[0] = 0;
  auto payload = "getCAN 1 " + encode_record(changed);
  auto corrupt = frame(payload);
  corrupt[corrupt.size() - 2] = corrupt[corrupt.size() - 2] == '0' ? '1' : '0';
  CHECK(bus.wire(corrupt).empty());
  CHECK(bus.ping() == "ackCAN 0");
  CHECK(bus.session.bank().find(5)->data == original);
  bus.receive(payload + " invalid");
  CHECK(bus.ping() == "ackCAN 0");
  CHECK(bus.session.bank().find(5)->data == original);
  bus.receive("getCAN 0 " + encode_record(changed));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.bank().find(5)->data == original);
  bus.receive(payload);
  CHECK(bus.session.bank().find(5)->data[0] == 0);
}

static void test_retry_and_discovery() {
  Bus bus;
  CHECK(bus.ping() == "getSystemData");
  // A missing negotiation reply or silent dirty reset must not block discovery.
  CHECK(bus.ping() == "setCAN 0701000000600000000000000");
  const std::string dump = "setCAN 0801000000600000000000000 0801000000236000000000000";
  CHECK(bus.ping() == dump);
  for (int attempt = 0; attempt < 2; attempt++) {
    bus.receive("getCAN 0 ");
    CHECK(bus.ping() == "ackCAN 1");
    CHECK(bus.ping() == dump);
  }
  bus.receive("getCAN 0 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "getSystemData");
  CHECK(!bus.session.ready());

  Bus missing;
  start(missing);
  const auto flush = "setCAN 0701000000600000000000000";
  CHECK(missing.wire("<U>getCAN 1 incomplete").empty());
  CHECK(missing.ping() == flush);
  CHECK(missing.ping() == flush);
  CHECK(missing.ping() == "getSystemData");
}

static void test_disconnect_and_restart() {
  Bus bus;
  bus.now = UINT32_MAX - 500;
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.session.ready());
  bus.now += 1000;
  bus.session.tick(bus.now);
  CHECK(bus.session.ready());
  bus.now += 5000;
  bus.session.tick(bus.now);
  CHECK(!bus.session.ready());
  CHECK(bus.session.bank().find(5) == nullptr);
  start(bus);
  CHECK(!bus.session.ready());
  bus.receive(snapshot(1));
  CHECK(bus.session.ready());
  CHECK(bus.session.bank().find(3, 2) == nullptr);
  CHECK(!Session{}.ready());
}

static void test_snapshot_timeout() {
  Bus bus;
  start(bus);
  for (int i = 0; i < 12; i++) {
    bus.now += 900;
    bus.receive("getCAN 1 ");
    bus.ping();
  }
  CHECK(!bus.session.ready());
  CHECK(!bus.session.bank().has_control_box());
}

static void test_sensor_bindings() {
  Bus bus;
  bus.session.set_sensor(1, 0x012345);
  bus.session.set_sensor(3, 0xABCDEF);
  bus.session.set_sensor(4, 0x111111);
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.ping() == "ackCAN 1");
  const std::string bindings = "setCAN 0701abcde1201234501000000 0701abcde12abcdef03000000";
  CHECK(bus.ping() == bindings);
  bus.receive("getCAN 0 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == bindings);
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN ");

  auto request = *bus.session.bank().find(5);
  request.kind = 0x13;
  request.unit_id = 0x11111;
  bus.receive("getCAN 1 " + encode_record(request));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN ");
  request.unit_id = 0xabcde;
  bus.receive("getCAN 1 " + encode_record(request));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == bindings);

  bus.now += 5000;
  bus.session.tick(bus.now);
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == bindings);
}

static void test_sensor_binding_batches() {
  Bus bus;
  std::vector<Record> expected;
  for (uint8_t zone = 1; zone <= 26; zone++) {
    bus.session.set_sensor(zone, 0x123400 + zone);
    auto binding = request(7, 0xabcde, 0x12);
    binding.data = {0x12, 0x34, zone, zone, 0, 0, 0};
    expected.push_back(binding);
  }
  const auto last = expected.back();
  expected.pop_back();
  start(bus);
  bus.receive(snapshot(26));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == set_can(expected));
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == set_can({last}));
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN ");
}

static Record pairing_result(uint8_t zone = 1, uint32_t uid = 0x01186E, uint8_t status = 2) {
  auto record = request(8, 0xabcde, 0x26);
  record.direction = 3;
  record.data = {
      status, 4, zone, static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 8), static_cast<uint8_t>(uid), 0};
  return record;
}

static void test_sensor_pairing() {
  Bus bus;
  CHECK(!bus.session.start_pairing(1, bus.now));
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(!bus.session.start_pairing(0, bus.now));
  CHECK(!bus.session.start_pairing(4, bus.now));
  const auto announce = [&](const Record &record) {
    bus.receive("getCAN 1 " + encode_record(record));
    CHECK(bus.ping() == "ackCAN 1");
  };
  announce(pairing_result());
  CHECK(!bus.session.sensor_uid(1));
  CHECK(bus.session.start_pairing(1, bus.now));
  announce(pairing_result());
  CHECK(!bus.session.sensor_uid(1));
  const std::string pair = "setCAN 0701abcde2601810100000000";
  CHECK(bus.ping() == pair);
  bus.receive("getCAN 0 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == pair);

  announce(pairing_result(1, 0));
  announce(pairing_result(2));
  announce(pairing_result(1, 0x01186E, 1));
  auto record = pairing_result();
  record.data[1] = 2;
  announce(record);
  record = pairing_result();
  record.unit_id = 0x11111;
  announce(record);
  record = pairing_result();
  record.unit_type = 9;
  announce(record);
  record = pairing_result();
  record.direction = 1;
  announce(record);
  CHECK(bus.session.pairing_zone() == 1 && !bus.session.sensor_uid(1));

  announce(pairing_result(1, 0xA1186E));
  CHECK(bus.session.sensor_uid(1) == 0x01186E);
  CHECK(bus.session.pairing_zone() == 0);
  CHECK(bus.ping() == "setCAN 0701abcde1201186e01000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  announce(pairing_result(1, 0x12345));
  CHECK(bus.session.sensor_uid(1) == 0x01186E);
  CHECK(bus.ping() == "setCAN ");

  CHECK(bus.session.start_pairing(2, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810200000000");
  announce(pairing_result(2));
  CHECK(bus.session.sensor_uid(1) == 0);
  CHECK(bus.session.sensor_uid(2) == 0x01186E);
  CHECK(bus.ping() == "setCAN 0701abcde1200000001000000 0701abcde1201186e02000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  bus.session.set_sensor(2, 0);
  CHECK(bus.ping() == "setCAN 0701abcde1200000001000000 0701abcde1200000002000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  bus.now += 5000;
  bus.session.tick(bus.now);
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN 0701abcde1200000001000000 0701abcde1200000002000000");
}

static void test_probe_button_pairing() {
  Bus bus;
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.ping() == "ackCAN 1");
  const auto received = [&](const std::string &record) {
    bus.receive("getCAN 1 " + record);
    CHECK(bus.ping() == "ackCAN 1");
  };
  received("0703abcde1201186e40070000");
  CHECK(!bus.session.sensor_uid(3));
  CHECK(bus.session.start_pairing(3, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810300000000");
  received("0804abcde26018103ffffff00");
  received("0703abcde1201186e00070000");
  received("0703abcde1200000040070000");
  received("0703111111201186e40070000");
  CHECK(!bus.session.sensor_uid(3));
  received("0703abcde1201186e40070000");
  CHECK(bus.session.sensor_uid(3) == 0x01186E);
  CHECK(bus.session.pairing_zone() == 0);
  CHECK(bus.ping() == "setCAN 0701abcde2600810300000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN 0701abcde1201186e03000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");

  CHECK(bus.session.start_pairing(3, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810300000000");
  received("0804abcde26038103ffffff00");
  CHECK(bus.session.pairing_zone() == 0);
  CHECK(bus.session.sensor_uid(3) == 0x01186E);
  received("0703abcde1201234540070000");
  CHECK(bus.session.sensor_uid(3) == 0x01186E);
}

static void test_control_box_pairing_reply() {
  Bus bus;
  start(bus);
  bus.receive(snapshot(3));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.start_pairing(3, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810300000000");
  bus.receive("getCAN 1 0804abcde26018103ffffff00");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.pairing_zone() == 3 && !bus.session.sensor_uid(3));
  bus.receive("getCAN 1 0804abcde26028103ffffff00");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.pairing_zone() == 3 && !bus.session.sensor_uid(3));
  bus.receive("getCAN 1 0804abcde2602810301186e00 0703abcde1201186e40050000");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.pairing_zone() == 0);
  CHECK(bus.session.sensor_uid(3) == 0x01186E);
  CHECK(bus.ping() == "setCAN 0701abcde1201186e03000000");
}

static void test_pairing_cancellation() {
  Bus bus;
  bus.now = UINT32_MAX - 1000;
  start(bus);
  bus.receive(snapshot(2));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.start_pairing(1, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810100000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.start_pairing(2, bus.now));
  bus.session.cancel_pairing(1);
  CHECK(bus.session.pairing_zone() == 2);
  CHECK(bus.ping() == "setCAN 0701abcde2600810100000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN 0701abcde2601810200000000");
  const auto started = bus.now;
  while (bus.now - started < 58000) {
    bus.now += 500;
    bus.receive("getCAN 1 ");
    CHECK(bus.ping() == "ackCAN 1");
  }
  CHECK(bus.session.pairing_zone() == 2);
  bus.now = started + 60000;
  bus.receive("getCAN 1 " + encode_record(pairing_result(2)));
  CHECK(bus.session.ready());
  CHECK(bus.session.pairing_zone() == 0 && !bus.session.sensor_uid(2));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN 0701abcde2600810200000000");
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");

  CHECK(bus.session.start_pairing(1, bus.now));
  bus.session.cancel_pairing(1);
  CHECK(bus.session.pairing_zone() == 0);
  CHECK(bus.session.start_pairing(1, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810100000000");
  bus.receive("getCAN 1 " + encode_record(pairing_result(1, 0, 3)));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.session.pairing_zone() == 0 && !bus.session.sensor_uid(1));

  CHECK(bus.session.start_pairing(1, bus.now));
  CHECK(bus.ping() == "setCAN 0701abcde2601810100000000");
  bus.now += 5000;
  bus.session.tick(bus.now);
  CHECK(bus.session.pairing_zone() == 0);
  start(bus);
  bus.receive(snapshot(2));
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == "setCAN 0701abcde2600810100000000");
  CHECK(!bus.session.sensor_uid(1));
}

void test_controls();
void test_activity();

int main() {
  test_frames();
  test_records();
  test_captured_snapshot();
  test_sensor_report_counter();
  test_zone_counts_and_partial_snapshot();
  test_errors_do_not_change_state();
  test_retry_and_discovery();
  test_disconnect_and_restart();
  test_snapshot_timeout();
  test_sensor_bindings();
  test_sensor_binding_batches();
  test_sensor_pairing();
  test_probe_button_pairing();
  test_control_box_pairing_reply();
  test_pairing_cancellation();
  test_controls();
  test_activity();
  std::cout << "All tests passed\n";
}
