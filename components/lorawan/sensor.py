import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID

from . import CONF_LORAWAN_ID, LoRaWANComponent, lorawan_ns

# A payload field binds an existing ESPHome sensor to a slot in the uplink. The
# encoding is intentionally tiny for the spike (float32, little-endian, appended
# in declaration order); the ChirpStack decodeUplink codec is generated from the
# same ordered list so the two stay in lockstep.
PayloadField = lorawan_ns.class_("PayloadField")

CONF_SENSOR = "sensor"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PayloadField),
        cv.GenerateID(CONF_LORAWAN_ID): cv.use_id(LoRaWANComponent),
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LORAWAN_ID])
    src = await cg.get_variable(config[CONF_SENSOR])
    cg.add(parent.add_payload_field(src))
