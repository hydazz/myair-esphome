import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

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
            self.assertEqual(zone["climate"]["name"], f"Zone {n}")
            self.assertEqual(zone["temperature"]["name"], f"Zone {n} Temperature")
            self.assertEqual(zone["damper"]["name"], f"Zone {n} Damper")
            for key in ("min_damper", "max_damper", "temperature_offset", "sensor_id"):
                self.assertNotIn(key, zone)
        for key in ("reference_zone", "ac_mode", "ac_fan", "connected"):
            self.assertNotIn(key, config["myair"])
        for domain, count in (
            ("climate", 11),
            ("sensor", 20),
            ("number", 0),
            ("text_sensor", 0),
            ("binary_sensor", 0),
            ("switch", 0),
            ("button", 0),
        ):
            self.assertEqual(CORE.cpp_main_section.count(f"App.register_{domain}("), count)

    def test_device_zones_take_names_from_their_devices(self):
        config = self.valid(
            BASE.replace(
                "  name: config-test\n",
                """  name: config-test
  areas:
    - id: living_room
      name: Living Room
  devices:
    - id: living_room_zone
      name: Living Room Zone
      area_id: living_room
""",
            )
            + """
myair:
  zones:
    - zone: 1
      device_id: living_room_zone
      sensor_id: {}
"""
        )
        generate_cpp_contents(config)
        zone = config["myair"]["zones"][0]
        self.assertNotIn("name", zone)
        self.assertEqual(zone["climate"]["name"], "")
        self.assertEqual(zone["temperature"]["name"], "Temperature")
        self.assertEqual(zone["sensor_id"]["name"], "Sensor UID")
        for key in ("climate", "temperature", "damper", "sensor_id"):
            self.assertEqual(str(zone[key]["device_id"]), "living_room_zone")
        self.assertIn('1, "Living Room Zone", true);', CORE.cpp_main_section)
        self.assertEqual(CORE.cpp_main_section.count("set_device_(living_room_zone)"), 4)

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
  reference_zone:
    id: reference
    icon: mdi:home-thermometer
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
        self.assertEqual(hub["reference_zone"]["name"], "Reference Zone")
        self.assertEqual(str(hub["reference_zone"]["id"]), "reference")
        self.assertEqual(zone["climate"]["name"], "Living Room")
        self.assertEqual(zone["temperature"]["accuracy_decimals"], 2)
        self.assertEqual(zone["temperature"]["filters"][0]["throttle"].total_seconds, 10)
        self.assertEqual(zone["damper"]["name"], "Living Room Airflow")
        self.assertEqual(zone["temperature_offset"]["name"], "Living Room Temperature Offset")
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
        for domain in (
            "climate",
            "sensor",
            "number",
            "switch",
            "binary_sensor",
            "text_sensor",
            "button",
        ):
            self.assertNotIn(f"App.register_{domain}(", CORE.cpp_main_section)
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
        self.assertEqual(zone["cover"]["name"], "Garage")
        self.assertEqual(zone["cover"]["device_class"], "damper")
        for key in ("climate", "temperature", "damper", "min_damper", "max_damper"):
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

    def test_remote_source_loads_component_without_a_package(self):
        def checkout(**kwargs):
            self.assertEqual(
                kwargs["url"].removesuffix(".git"), "https://github.com/hydazz/myair-esphome"
            )
            self.assertEqual(kwargs["ref"], "main")
            return ROOT, None

        with patch("esphome.git.clone_or_update", side_effect=checkout) as fetch:
            config = self.valid(
                DEVICE
                + """
external_components:
  - source: github://hydazz/myair-esphome@main
    components: [myair]
myair:
  zones:
    - zone: 1
      name: Living Room
"""
            )
        generate_cpp_contents(config)
        self.assertEqual(len(config["myair"]["zones"]), 1)
        self.assertEqual(
            {call.kwargs["domain"] for call in fetch.call_args_list}, {"external_components"}
        )

    def test_zone_definitions_reject_conflicts(self):
        cases = (
            ("    - zone: 0\n", "at least 1"),
            ("    - zone: 11\n", "at most 10"),
            (
                "    - zone: 1\n      name: Living\n    - zone: 1\n      name: Bedroom\n",
                "configured once",
            ),
            ("    - zone: 1\n      name: Zone 2\n    - zone: 2\n", "Duplicate"),
            ("    - zone: 1\n      device_id: missing\n", "missing"),
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

    def test_invalid_nested_options_are_rejected(self):
        for entity in (
            "temperature: true",
            "temperature: [Temperature]",
            "temperature: {filters: [{unknown_filter: 1}]}",
            "temperature: {unknown_option: 1}",
            "pair_sensor: {inverted: true}",
            'name: ""',
            "airflow_weight: 0",
            "airflow_weight: -1",
            "airflow_weight: 8in",
            "airflow_weight: [1, 2]",
        ):
            with self.subTest(entity=entity):
                config = self.load(BASE + "myair:\n  zones:\n    - zone: 1\n      " + entity + "\n")
                self.assertTrue(config.errors)

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
        for key in ("reference_zone", "ac_mode", "ac_fan", "connected"):
            self.assertEqual(hub[key]["entity_category"], "diagnostic")
        self.assertEqual(hub["climate"]["name"], "")
        self.assertIn("set_step(5", CORE.cpp_main_section)
        self.assertIn("->set_airflow_weight(1, 2", CORE.cpp_main_section)
        self.assertEqual(hub["zones"][0]["climate"]["name"], "Living Room")
        self.assertEqual(hub["zones"][2]["climate"]["name"], "Study")
        self.assertEqual(hub["zones"][3]["cover"]["name"], "Garage")


if __name__ == "__main__":
    unittest.main()
