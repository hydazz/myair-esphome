#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

#include "controller.h"
#include "protocol.h"
#include "session.h"

using namespace esphome::myair;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << __func__ << ':' << __LINE__ << ": " << #condition << '\n'; \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

struct Bus {
  Session session;
  uint32_t now{100};

  std::string wire(const std::string &input) {
    std::string output;
    for (uint8_t byte : input) output += session.receive(byte, now);
    return output;
  }
  void receive(const std::string &payload) { CHECK(wire(frame(payload)).empty()); }
  std::string ping() {
    now += 43;
    auto output = wire("<U>Ping</U=db>");
    if (output.empty()) return {};
    CHECK(output.substr(0, 3) == "<U>");
    CHECK(output.find("<U>", 3) == std::string::npos);
    return output.substr(3, output.size() - 10);
  }
};

inline std::string fixture() {
  std::ifstream file("fixtures/snapshot.txt");
  CHECK(file.good());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

inline std::string snapshot(uint8_t zones, uint8_t type = 7) {
  std::vector<Record> records;
  auto config = request(type, 0xabcde, 1);
  config.direction = 3;
  config.data = {0x20, zones, 0, 0, 0, 0, 0};
  records.push_back(config);
  auto system = config;
  system.kind = 5;
  system.data = {1, 1, 2, 48, 1, 1, 0};
  records.push_back(system);
  for (unsigned zone = 1; zone <= zones; zone++) {
    auto state = config;
    state.kind = 3;
    state.data = {static_cast<uint8_t>(zone), 0xB2, 1, 48, 24, 5, 0};
    records.push_back(state);
    auto settings = state;
    settings.kind = 4;
    settings.data = {static_cast<uint8_t>(zone), 0, 100, 0, 1, 0, 64};
    records.push_back(settings);
  }
  std::string payload = "getCAN 1 ";
  for (const auto &record : records) payload += encode_record(record) + ' ';
  return payload;
}

inline void start(Bus &bus, uint8_t type = 7) {
  CHECK(bus.ping() == "getSystemData");
  bus.receive("CAN2 in use");
  CHECK(bus.ping() == "setCAN 0701000000600000000000000");
  auto announcement = request(type, 0xabcde, 0x0A);
  announcement.direction = 3;
  auto firmware = announcement;
  firmware.kind = 6;
  firmware.data = {10, 30, 1, 4, 0, 0, 0};
  bus.receive("getCAN 1 " + encode_record(announcement) + " " + encode_record(firmware) + " ");
  CHECK(!bus.session.ready());
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == set_can({request(type, 0xabcde, 7)}));
  bus.receive("getCAN 1 ");
  CHECK(bus.ping() == "ackCAN 1");
  CHECK(bus.ping() == set_can({request(type, 0, 6)}));
}
