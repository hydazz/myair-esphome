#pragma once

#include <cstdint>
#include <optional>

namespace esphome::myair {

enum class Mode : uint8_t { OFF, AUTO, COOL, HEAT, FAN, DRY };
enum class Fan : uint8_t { AUTO, LOW, MEDIUM, HIGH };
enum class Action : uint8_t { OFF, IDLE, COOLING, HEATING, FAN, DRYING };

struct Room {
  bool on{};
  float heat_to{20};
  float cool_to{24};
};

struct Airflow {
  bool on{};
  uint8_t position{100};
};

// The CB10's enable flag does not indicate whether the compressor is running.
struct EquipmentState {
  bool enabled{};
  std::optional<Mode> mode;
  std::optional<Fan> fan;
  uint8_t reference{};
};

}  // namespace esphome::myair
