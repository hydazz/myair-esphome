import re
import tempfile
import unittest
from pathlib import Path

from esphome.__main__ import generate_cpp_contents
from esphome.config import load_config
from esphome.core import CORE

ROOT = Path(__file__).resolve().parents[1]
DEVICE = """
esphome:
  name: config-test
esp32:
  board: m5stack-atoms3
  framework:
    type: esp-idf
uart:
  id: cb_uart
  tx_pin: GPIO6
  rx_pin: GPIO5
  baud_rate: 57600
"""
BASE = (
    DEVICE
    + f"""
external_components:
  - source:
      type: local
      path: {ROOT / "components"}
"""
)


class ConfigurationTest(unittest.TestCase):
    def load(self, yaml):
        CORE.reset()
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "config.yaml"
        path.write_text(yaml)
        CORE.config_path = path
        return load_config({})

    def valid(self, yaml):
        config = self.load(yaml)
        self.assertEqual(config.errors, [])
        CORE.config = config
        return config

    def test_ten_zones_create_only_standard_entities(self):
        zones = "".join(f"    - zone: {n}\n" for n in range(1, 11))
        config = self.valid(BASE + "myair:\n  zones:\n" + zones)
        generate_cpp_contents(config)
        for n, zone in enumerate(config["myair"]["zones"], 1):
            self.assertEqual(zone["name"], f"Zone {n}")
            self.assertEqual(zone["climate"]["name"], "Climate")
            self.assertEqual(zone["temperature"]["name"], "Temperature")
            self.assertEqual(zone["damper"]["name"], "Damper")
            probe = zone["diagnostics"]
            self.assertEqual(probe["low_battery"]["name"], "Low Battery")
            self.assertFalse(probe["low_battery"]["disabled_by_default"])
            for key in ("sensor_report", "sensor_error"):
                self.assertTrue(probe[key]["disabled_by_default"])
            for key in ("min_damper", "max_damper", "temperature_offset", "sensor_id"):
                self.assertNotIn(key, zone)
        system = config["myair"]["diagnostics"]
        for key in ("reference_zone", "ac_mode", "ac_fan", "connected", "ac_error"):
            self.assertFalse(system[key]["disabled_by_default"])
        self.assertTrue(system["write_failures"]["disabled_by_default"])
        for domain, count in (
            ("climate", 11),
            ("sensor", 41),
            ("number", 0),
            ("text_sensor", 5),
            ("binary_sensor", 11),
            ("switch", 0),
            ("button", 0),
        ):
            self.assertEqual(CORE.cpp_main_section.count(f"App.register_{domain}("), count)

    def test_zones_register_devices_without_areas(self):
        config = self.valid(BASE + "myair:\n  zones:\n    - zone: 1\n      name: Living Room\n")
        generate_cpp_contents(config)
        devices = config["esphome"]["devices"]
        self.assertEqual(
            [(str(d["id"]), d["name"]) for d in devices], [("myair_zone_1", "Living Room")]
        )
        self.assertEqual(config["esphome"]["areas"], [])
        self.assertNotIn("area_id", devices[0])
        self.assertNotIn("set_area_id(", CORE.cpp_main_section)
        self.assertIn("App.register_device(myair_zone_1)", CORE.cpp_main_section)
        self.assertEqual(CORE.cpp_main_section.count("set_device_(myair_zone_1)"), 6)
        self.assertNotIn("device_id", config["myair"]["climate"])
        for diagnostic in config["myair"]["diagnostics"].values():
            self.assertNotIn("device_id", diagnostic)

    def test_generated_devices_coexist_with_other_devices(self):
        config = self.valid(
            BASE.replace(
                "  name: config-test\n",
                """  name: config-test
  devices:
    - id: utility
      name: Utility
""",
            )
            + """
sensor:
  - platform: template
    name: Temperature
    device_id: utility
myair:
  zones:
    - zone: 7
    - zone: 2
"""
        )
        generate_cpp_contents(config)
        self.assertEqual(
            [str(d["id"]) for d in config["esphome"]["devices"]],
            ["utility", "myair_zone_7", "myair_zone_2"],
        )
        self.assertEqual(CORE.cpp_main_section.count("App.register_device("), 3)
        defines = {d.name: str(d.value) for d in CORE.defines}
        self.assertEqual(defines["ESPHOME_DEVICE_COUNT"], "3")
        self.assertNotIn("USE_AREAS", defines)
        self.assertIn("set_device_(utility)", CORE.cpp_main_section)

    def test_zone_renaming_and_order_keep_device_and_entity_keys(self):
        def identity(zones):
            config = self.valid(BASE + "myair:\n  zones:\n" + zones)
            generate_cpp_contents(config)
            cpp = CORE.cpp_main_section
            devices = sorted(re.findall(r"(myair_zone_\d+)->set_device_id\((\d+)(?:UL)?\)", cpp))
            entities = sorted(re.findall(r'App.register_(\w+)\(\w+, "([^"]*)", (\d+)(?:UL)?,', cpp))
            self.assertEqual(len(devices), 2)
            self.assertTrue(entities)
            return devices, entities

        before = identity(
            "    - zone: 1\n      name: Living Room\n    - zone: 7\n      sensor: false\n"
        )
        after = identity(
            "    - zone: 7\n      name: Garage\n      sensor: false\n"
            "    - zone: 1\n      name: Lounge\n"
        )
        self.assertEqual(before, after)

    def test_zone_entities_cannot_override_their_device(self):
        config = self.load(
            BASE + "myair:\n  zones:\n    - zone: 1\n      climate: {device_id: other}\n"
        )
        self.assertIn("Zone entities belong to their zone device", str(config.errors))

    def test_sparse_zones_keep_physical_numbers(self):
        config = self.valid(
            BASE
            + """
myair:
  zones:
    - zone: 1
      name: Living Room
      sensor_uid: "01186E"
    - zone: 7
      name: Office
    - zone: 10
"""
        )
        generate_cpp_contents(config)
        zones = config["myair"]["zones"]
        self.assertEqual([zone["zone"] for zone in zones], [1, 7, 10])
        self.assertEqual(zones[0]["sensor_uid"], "01186e")
        for number, name in ((1, "Living Room"), (7, "Office"), (10, "Zone 10")):
            self.assertIn(f'{number}, "{name}", true);', CORE.cpp_main_section)
        self.assertIn("->set_sensor(1, 71790)", CORE.cpp_main_section)
        self.assertEqual(CORE.cpp_main_section.count("->add_zone("), 3)

    def test_nested_entities_accept_native_options_and_name_shorthand(self):
        config = self.valid(
            BASE
            + """
myair:
  climate: Ducted AC
  zones:
    - zone: 1
      name: Living Room
      airflow_weight: 2.5
      climate:
        id: living_climate
        visual:
          max_temperature: 28
      temperature:
        id: living_temperature
        name: Sitting Room Temperature
        accuracy_decimals: 2
        filters:
          - throttle: 10s
      damper: Living Room Airflow
      temperature_offset: {}
"""
        )
        generate_cpp_contents(config)
        hub = config["myair"]
        zone = hub["zones"][0]
        self.assertEqual(hub["climate"]["name"], "Ducted AC")
        self.assertEqual(zone["climate"]["name"], "Climate")
        self.assertEqual(zone["temperature"]["accuracy_decimals"], 2)
        self.assertEqual(zone["temperature"]["filters"][0]["throttle"].total_seconds, 10)
        self.assertEqual(zone["damper"]["name"], "Living Room Airflow")
        self.assertEqual(zone["temperature_offset"]["name"], "Temperature Offset")
        self.assertIn("->set_airflow_weight(1, 2.5", CORE.cpp_main_section)
        self.assertIn(
            "living_climate->set_visual_max_temperature_override(28", CORE.cpp_main_section
        )
        self.assertIn(
            'App.register_sensor(living_temperature, "Sitting Room Temperature"',
            CORE.cpp_main_section,
        )

    def test_defaults_can_be_disabled_without_removing_zone_state(self):
        config = self.valid(
            BASE
            + """
myair:
  climate: false
  zones:
    - zone: 6
      sensor_uid: "01186e"
      climate: false
      temperature: false
      damper: false
"""
        )
        generate_cpp_contents(config)
        for domain in ("climate", "number", "switch", "button"):
            self.assertNotIn(f"App.register_{domain}(", CORE.cpp_main_section)
        self.assertEqual(CORE.cpp_main_section.count("App.register_sensor("), 3)
        self.assertEqual(CORE.cpp_main_section.count("->add_zone("), 1)
        self.assertIn("->set_sensor(6, 71790)", CORE.cpp_main_section)

    def test_zones_without_a_sensor_get_a_damper_cover(self):
        config = self.valid(
            BASE
            + """
myair:
  zones:
    - zone: 3
      name: Garage
      sensor: false
      airflow_weight: 0.5
"""
        )
        generate_cpp_contents(config)
        zone = config["myair"]["zones"][0]
        self.assertEqual(zone["cover"]["name"], "Damper")
        self.assertEqual(zone["cover"]["device_class"], "damper")
        for key in ("climate", "temperature", "damper", "diagnostics", "min_damper", "max_damper"):
            self.assertNotIn(key, zone)
        self.assertIn('3, "Garage", false);', CORE.cpp_main_section)
        self.assertIn("->set_airflow_weight(3, 0.5", CORE.cpp_main_section)
        self.assertIn("App.register_cover(", CORE.cpp_main_section)
        self.assertNotIn("App.register_climate(myair_zoneclimate", CORE.cpp_main_section)

    def test_sensor_options_need_a_sensor(self):
        for zone in (
            '      sensor: false\n      sensor_uid: "01186e"\n',
            "      sensor: false\n      climate: Garage\n",
            "      sensor: false\n      temperature_offset: {}\n",
            "      sensor: false\n      low_battery: {}\n",
            "      cover: Garage\n",
        ):
            with self.subTest(zone=zone):
                config = self.load(BASE + "myair:\n  zones:\n    - zone: 3\n" + zone)
                self.assertTrue(config.errors)

    def test_system_without_zones_has_no_phantom_zones(self):
        config = self.valid(BASE + "myair:\n")
        generate_cpp_contents(config)
        self.assertEqual(config["myair"]["zones"], [])
        self.assertNotIn("->add_zone(", CORE.cpp_main_section)
        self.assertEqual(CORE.cpp_main_section.count("App.register_climate("), 1)

    def test_zone_definitions_reject_conflicts(self):
        cases = (
            ("    - zone: 0\n", "at least 1"),
            ("    - zone: 11\n", "at most 10"),
            (
                "    - zone: 1\n      name: Living\n    - zone: 1\n      name: Bedroom\n",
                "Duplicate",
            ),
            ('    - zone: 1\n      sensor_uid: "000000"\n', "nonzero six-digit"),
            ('    - zone: 1\n      sensor_uid: "01186"\n', "nonzero six-digit"),
            (
                '    - zone: 1\n      sensor_uid: "01186E"\n'
                '    - zone: 2\n      sensor_uid: "01186e"\n',
                "assigned to one zone",
            ),
        )
        for zones, error in cases:
            with self.subTest(zones=zones):
                config = self.load(BASE + "myair:\n  zones:\n" + zones)
                self.assertIn(error, str(config.errors))

    def test_firmware_registers_optional_entities(self):
        yaml = (ROOT / "tests/firmware.yaml").read_text()
        yaml = yaml.replace("../components", str(ROOT / "components"))
        config = self.valid(yaml)
        generate_cpp_contents(config)
        for domain in (
            "climate",
            "cover",
            "sensor",
            "binary_sensor",
            "text_sensor",
            "number",
            "switch",
            "button",
        ):
            self.assertIn(f"App.register_{domain}(", CORE.cpp_main_section)
        hub = config["myair"]
        for entity in hub["diagnostics"].values():
            self.assertEqual(entity["entity_category"], "diagnostic")
        self.assertEqual(hub["climate"]["name"], "")
        for zone in hub["zones"]:
            expected = f"myair_zone_{zone['zone']}"
            for key in (
                "climate",
                "cover",
                "temperature",
                "damper",
                "temperature_offset",
                "min_damper",
                "max_damper",
                "pair_sensor",
                "unpair_sensor",
                "sensor_id",
            ):
                if zone.get(key):
                    self.assertEqual(str(zone[key]["device_id"]), expected)
            for diagnostic in zone.get("diagnostics", {}).values():
                self.assertEqual(str(diagnostic["device_id"]), expected)
        self.assertIn("set_step(5", CORE.cpp_main_section)
        self.assertIn("->set_airflow_weight(1, 2", CORE.cpp_main_section)
        self.assertEqual(hub["zones"][0]["climate"]["name"], "Climate")
        self.assertEqual(hub["zones"][2]["climate"]["name"], "Climate")
        self.assertEqual(hub["zones"][3]["cover"]["name"], "Damper")

    def test_power_feedback_uses_an_existing_sensor(self):
        config = self.valid(
            BASE
            + """
sensor:
  - platform: template
    id: ac_power
myair:
  power_feedback:
    sensor: ac_power
    idle_below: 50 W
"""
        )
        generate_cpp_contents(config)
        self.assertIn("->set_power_feedback(ac_power, 50.0f)", CORE.cpp_main_section)
        self.assertEqual(
            config["myair"]["diagnostics"]["activity_feedback"]["name"], "Activity Feedback"
        )

    def test_power_feedback_rejects_invalid_thresholds(self):
        for settings in (
            "idle_below: 0 W",
            "idle_below: -1 W",
            "idle_below: .nan",
            "idle_below: .inf",
        ):
            with self.subTest(settings=settings):
                config = self.load(
                    BASE
                    + """
sensor:
  - platform: template
    id: ac_power
myair:
  power_feedback:
    sensor: ac_power
    """
                    + settings
                    + "\n"
                )
                self.assertTrue(config.errors)


if __name__ == "__main__":
    unittest.main()
