import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_TYPE

from . import CONF_LORAWAN_ID, LoRaWANComponent, lorawan_ns

# A payload field binds an existing ESPHome sensor to a slot in the uplink,
# appended in declaration order with the encoding in payload.h. The ChirpStack
# codec (codec/decodeUplink.js) lists the same fields, types and multipliers.
PayloadField = lorawan_ns.class_("PayloadField")
FieldType = lorawan_ns.enum("FieldType", is_class=True)
FIELD_TYPES = {
    "float32": FieldType.FLOAT32,
    "int8": FieldType.INT8,
    "uint8": FieldType.UINT8,
    "int16": FieldType.INT16,
    "uint16": FieldType.UINT16,
    "int32": FieldType.INT32,
    "uint32": FieldType.UINT32,
}

CONF_SENSOR = "sensor"
CONF_MULTIPLIER = "multiplier"


def _validate_multiplier(config):
    if CONF_MULTIPLIER in config:
        if config[CONF_TYPE] == "float32":
            raise cv.Invalid(
                "multiplier applies to integer types only; float32 is sent as-is",
                path=[CONF_MULTIPLIER],
            )
        if config[CONF_MULTIPLIER] == 0:
            raise cv.Invalid("multiplier must be nonzero", path=[CONF_MULTIPLIER])
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PayloadField),
            cv.GenerateID(CONF_LORAWAN_ID): cv.use_id(LoRaWANComponent),
            cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_TYPE, default="float32"): cv.enum(FIELD_TYPES, lower=True),
            cv.Optional(CONF_MULTIPLIER): cv.float_,
        }
    ),
    _validate_multiplier,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LORAWAN_ID])
    src = await cg.get_variable(config[CONF_SENSOR])
    cg.add(
        parent.add_payload_field(
            src, FIELD_TYPES[config[CONF_TYPE]], config.get(CONF_MULTIPLIER, 1.0)
        )
    )
