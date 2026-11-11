import math
import re

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import (
    binary_sensor,
    button,
    climate,
    cover,
    number,
    sensor,
    switch,
    text_sensor,
    uart,
)
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DEVICES,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ESPHOME,
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DAMPER,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)
from esphome.core import AutoLoad
from esphome.core.config import DEVICE_SCHEMA, FRIENDLY_NAME_MAX_LEN, validate_ids_and_references

DOMAIN = "myair"
DEPENDENCIES = ["uart"]
AUTO_LOAD = [
    "binary_sensor",
    "button",
    "climate",
    "cover",
    "number",
    "sensor",
    "switch",
    "text_sensor",
]

myair_ns = cg.esphome_ns.namespace("myair")
MyAir = myair_ns.class_("MyAir", cg.Component, uart.UARTDevice)
Zone = myair_ns.class_("Zone")
SystemClimate = myair_ns.class_("SystemClimate", climate.Climate, cg.Component)
ZoneClimate = myair_ns.class_("ZoneClimate", climate.Climate, cg.Component)
ZoneCover = myair_ns.class_("ZoneCover", cover.Cover, cg.Component)
MyAirSensor = myair_ns.class_("MyAirSensor", sensor.Sensor)
SystemBinarySensor = myair_ns.class_("SystemBinarySensor", binary_sensor.BinarySensor)
ZoneLowBattery = myair_ns.class_("ZoneLowBattery", binary_sensor.BinarySensor)
MyAirTextSensor = myair_ns.class_("MyAirTextSensor", text_sensor.TextSensor)
ZoneNumber = myair_ns.class_("ZoneNumber", number.Number)
ZoneSwitch = myair_ns.class_("ZoneSwitch", switch.Switch)
ZoneButton = myair_ns.class_("ZoneButton", button.Button)
SensorType = myair_ns.enum("SensorType", is_class=True)
NumberType = myair_ns.enum("NumberType", is_class=True)
TextSensorType = myair_ns.enum("TextSensorType", is_class=True)

ZONE_ENTITY_NAMES = {
    "climate": "Climate",
    "cover": "Damper",
    "temperature": "Temperature",
    "damper": "Damper",
    "min_damper": "Minimum Damper",
    "max_damper": "Maximum Damper",
    "temperature_offset": "Temperature Offset",
    "pair_sensor": "Pair Sensor",
    "unpair_sensor": "Unpair Sensor",
    "sensor_id": "Sensor UID",
}
ZONE_DEFAULTS = ("climate", "temperature", "damper")
AIRFLOW_ZONE_DEFAULTS = ("cover",)
# Entities only a zone with a sensor can have.
SENSOR_ENTITIES = (
    "climate",
    "temperature",
    "temperature_offset",
    "pair_sensor",
    "unpair_sensor",
    "sensor_id",
)


def entity_names(config, names, defaults):
    config = config.copy()
    for key, name in names.items():
        if key not in config and key not in defaults:
            continue
        entity = config.get(key, {})
        if entity is None:
            entity = {}
        if isinstance(entity, str):
            entity = {CONF_NAME: entity}
        if isinstance(entity, dict):
            entity = {CONF_NAME: name, **entity}
        config[key] = entity
    return config


def system_defaults(config):
    if not isinstance(config, dict):
        return config
    return entity_names(config, {"climate": ""}, ("climate",))


def zone_defaults(config):
    if not isinstance(config, dict):
        return config
    defaults = ZONE_DEFAULTS if config.get("sensor", True) is not False else AIRFLOW_ZONE_DEFAULTS
    config = entity_names(config, ZONE_ENTITY_NAMES, defaults)
    if "zone" in config:
        with cv.prepend_path(["zone"]):
            config["zone"] = cv.int_(config["zone"])
    config.setdefault(CONF_NAME, f"Zone {config.get('zone', '')}")
    for key in ZONE_ENTITY_NAMES:
        if isinstance(entity := config.get(key), dict):
            if CONF_DEVICE_ID in entity:
                raise cv.Invalid(
                    "Zone entities belong to their zone device", path=[key, CONF_DEVICE_ID]
                )
            entity[CONF_DEVICE_ID] = f"myair_zone_{config.get('zone', '')}"
    return config


def zone_kind(config):
    if config["sensor"]:
        if config.get("cover"):
            raise cv.Invalid("cover is for zones without a sensor (sensor: false)", path=["cover"])
        return config
    for key in ("sensor_uid", *SENSOR_ENTITIES):
        if config.get(key):
            raise cv.Invalid(f"{key} needs a zone with a sensor", path=[key])
    return config


def entity_schema(schema):
    named = cv.maybe_simple_value(schema, key=CONF_NAME)

    def validate(value):
        return False if value is False else named(value)

    return validate


def sensor_uid(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9a-fA-F]{6}", value) or int(value, 16) == 0:
        raise cv.Invalid("sensor_uid must be a nonzero six-digit hexadecimal sensor ID")
    return value.lower()


DIAGNOSTIC_TEXT = text_sensor.text_sensor_schema(
    MyAirTextSensor, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
)
DIAGNOSTIC_DETAIL = sensor.sensor_schema(
    MyAirSensor, accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
).extend({cv.Optional(CONF_DISABLED_BY_DEFAULT, default=True): cv.boolean})
SYSTEM_DIAGNOSTICS = {
    "reference_zone": ("Reference Zone", DIAGNOSTIC_TEXT),
    "ac_mode": ("AC Mode", DIAGNOSTIC_TEXT),
    "ac_fan": ("AC Fan", DIAGNOSTIC_TEXT),
    "ac_error": ("AC Error", DIAGNOSTIC_TEXT),
    "activity_feedback": ("Activity Feedback", DIAGNOSTIC_TEXT),
    "connected": (
        "Connected",
        binary_sensor.binary_sensor_schema(
            SystemBinarySensor,
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    "write_failures": ("Unconfirmed Write Batches", DIAGNOSTIC_DETAIL),
}
PROBE_DIAGNOSTICS = {
    "low_battery": (
        "Low Battery",
        binary_sensor.binary_sensor_schema(
            ZoneLowBattery,
            device_class=DEVICE_CLASS_BATTERY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    "sensor_error": ("Sensor Error Flags", DIAGNOSTIC_DETAIL),
    "sensor_report": ("Sensor Report Counter", DIAGNOSTIC_DETAIL),
}


def diagnostics(definitions, zone=None):
    device = {CONF_DEVICE_ID: f"myair_zone_{zone['zone']}"} if zone is not None else {}
    return AutoLoad(
        (key, schema({CONF_NAME: name, **device})) for key, (name, schema) in definitions.items()
    )


def damper_limit():
    return entity_schema(
        number.number_schema(
            ZoneNumber, unit_of_measurement=UNIT_PERCENT, entity_category=ENTITY_CATEGORY_CONFIG
        )
    )


ZONE_SCHEMA = cv.All(
    zone_defaults,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Zone),
            cv.Required("zone"): cv.int_range(min=1, max=10),
            cv.Optional(CONF_NAME): cv.All(
                cv.string_no_slash, cv.Length(min=1), cv.ByteLength(max=FRIENDLY_NAME_MAX_LEN)
            ),
            cv.Optional("sensor", default=True): cv.boolean,
            cv.Optional("sensor_uid"): sensor_uid,
            cv.Optional("airflow_weight"): cv.float_range(min=0, min_included=False, max=100),
            cv.Optional("climate"): entity_schema(
                climate.climate_schema(ZoneClimate).extend(cv.COMPONENT_SCHEMA)
            ),
            cv.Optional("cover"): entity_schema(
                cover.cover_schema(ZoneCover, device_class=DEVICE_CLASS_DAMPER).extend(
                    cv.COMPONENT_SCHEMA
                )
            ),
            cv.Optional("temperature"): entity_schema(
                sensor.sensor_schema(
                    MyAirSensor,
                    unit_of_measurement=UNIT_CELSIUS,
                    accuracy_decimals=1,
                    device_class=DEVICE_CLASS_TEMPERATURE,
                    state_class=STATE_CLASS_MEASUREMENT,
                )
            ),
            cv.Optional("damper"): entity_schema(
                sensor.sensor_schema(
                    MyAirSensor,
                    unit_of_measurement=UNIT_PERCENT,
                    accuracy_decimals=0,
                    state_class=STATE_CLASS_MEASUREMENT,
                    icon="mdi:valve",
                )
            ),
            cv.Optional("min_damper"): damper_limit(),
            cv.Optional("max_damper"): damper_limit(),
            cv.Optional("temperature_offset"): entity_schema(
                number.number_schema(
                    ZoneNumber,
                    unit_of_measurement=UNIT_CELSIUS,
                    entity_category=ENTITY_CATEGORY_CONFIG,
                )
            ),
            cv.Optional("pair_sensor"): entity_schema(
                switch.switch_schema(
                    ZoneSwitch,
                    block_inverted=True,
                    default_restore_mode="DISABLED",
                    icon="mdi:access-point",
                    entity_category=ENTITY_CATEGORY_CONFIG,
                )
            ),
            cv.Optional("unpair_sensor"): entity_schema(
                button.button_schema(
                    ZoneButton, icon="mdi:link-off", entity_category=ENTITY_CATEGORY_CONFIG
                )
            ),
            cv.Optional("sensor_id"): entity_schema(
                text_sensor.text_sensor_schema(
                    MyAirTextSensor,
                    icon="mdi:identifier",
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                )
            ),
        }
    ),
    zone_kind,
)


def validate_zones(config):
    numbers = [zone["zone"] for zone in config["zones"]]
    if len(numbers) != len(set(numbers)):
        raise cv.Invalid("Each zone may only be configured once")
    sensors = [zone["sensor_uid"] for zone in config["zones"] if "sensor_uid" in zone]
    if len(sensors) != len(set(sensors)):
        raise cv.Invalid("Each sensor may only be assigned to one zone")
    return config


def add_diagnostics(config):
    config["diagnostics"] = diagnostics(SYSTEM_DIAGNOSTICS)
    for zone in config["zones"]:
        if zone["sensor"]:
            zone["diagnostics"] = diagnostics(PROBE_DIAGNOSTICS, zone)
    return config


def add_devices(config):
    for zone in config["zones"]:
        zone["device"] = AutoLoad(
            DEVICE_SCHEMA({CONF_ID: f"myair_zone_{zone['zone']}", CONF_NAME: zone[CONF_NAME]})
        )
    return config


def register_devices(config):
    # Core codegen owns device registration and the size of its static device array.
    core = fv.full_config.get()[CONF_ESPHOME]
    core[CONF_DEVICES].extend(zone["device"] for zone in config["zones"])
    validate_ids_and_references(core)
    return config


def idle_threshold(value):
    value = cv.float_with_unit("power", "(W|w)?")(value)
    if not math.isfinite(value) or value <= 0:
        raise cv.Invalid("idle_below must be finite and positive")
    return value


POWER_FEEDBACK_SCHEMA = cv.Schema(
    {
        cv.Required("sensor"): cv.use_id(sensor.Sensor),
        cv.Required("idle_below"): idle_threshold,
    }
)


CONFIG_SCHEMA = cv.All(
    system_defaults,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MyAir),
            cv.Optional("climate"): entity_schema(
                climate.climate_schema(SystemClimate).extend(cv.COMPONENT_SCHEMA)
            ),
            cv.Optional("zones", default=[]): cv.ensure_list(ZONE_SCHEMA),
            cv.Optional("power_feedback"): POWER_FEEDBACK_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    validate_zones,
    add_devices,
    add_diagnostics,
)

FINAL_VALIDATE_SCHEMA = cv.All(
    uart.final_validate_device_schema(
        DOMAIN,
        baud_rate=57600,
        require_rx=True,
        require_tx=True,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    ),
    register_devices,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if feedback := config.get("power_feedback"):
        source = await cg.get_variable(feedback["sensor"])
        cg.add(var.set_power_feedback(source, feedback["idle_below"]))

    if config.get("climate"):
        entity = await climate.new_climate(config["climate"], var)
        await cg.register_component(entity, config["climate"])
    for key in ("reference_zone", "ac_mode", "ac_fan", "ac_error", "activity_feedback"):
        await text_sensor.new_text_sensor(
            config["diagnostics"][key], var, getattr(TextSensorType, key.upper())
        )
    await binary_sensor.new_binary_sensor(config["diagnostics"]["connected"], var)
    await sensor.new_sensor(config["diagnostics"]["write_failures"], var, SensorType.WRITE_FAILURES)

    for zone_config in config["zones"]:
        number_ = zone_config["zone"]
        zone = cg.new_Pvariable(
            zone_config[CONF_ID], var, number_, zone_config[CONF_NAME], zone_config["sensor"]
        )
        if "sensor_uid" in zone_config:
            cg.add(var.set_sensor(number_, int(zone_config["sensor_uid"], 16)))
        if "airflow_weight" in zone_config:
            cg.add(var.set_airflow_weight(number_, zone_config["airflow_weight"]))
        cg.add(var.add_zone(zone))
        if zone_config.get("climate"):
            entity = await climate.new_climate(zone_config["climate"], zone)
            await cg.register_component(entity, zone_config["climate"])
        if zone_config.get("cover"):
            entity = await cover.new_cover(zone_config["cover"], zone)
            await cg.register_component(entity, zone_config["cover"])
        for key in ("temperature", "damper"):
            if zone_config.get(key):
                await sensor.new_sensor(zone_config[key], zone, getattr(SensorType, key.upper()))
        if zone_config["sensor"]:
            probe = zone_config["diagnostics"]
            await binary_sensor.new_binary_sensor(probe["low_battery"], zone)
            for key in ("sensor_error", "sensor_report"):
                await sensor.new_sensor(probe[key], zone, getattr(SensorType, key.upper()))
        for key, minimum, maximum, step in (
            ("temperature_offset", -5, 5, 0.1),
            ("min_damper", 0, 100, 5),
            ("max_damper", 0, 100, 5),
        ):
            if zone_config.get(key):
                await number.new_number(
                    zone_config[key],
                    zone,
                    getattr(NumberType, key.upper()),
                    min_value=minimum,
                    max_value=maximum,
                    step=step,
                )
        if zone_config.get("pair_sensor"):
            await switch.new_switch(zone_config["pair_sensor"], zone)
        if zone_config.get("unpair_sensor"):
            await button.new_button(zone_config["unpair_sensor"], zone)
        if zone_config.get("sensor_id"):
            await text_sensor.new_text_sensor(
                zone_config["sensor_id"], zone, TextSensorType.SENSOR_ID
            )
