import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@moellere"]
# sensor bindings are an optional sub-platform; the component works without any.
AUTO_LOAD = ["sensor"]
MULTI_CONF = False

lorawan_ns = cg.esphome_ns.namespace("lorawan")
LoRaWANComponent = lorawan_ns.class_("LoRaWANComponent", cg.Component)

# Used by the sensor sub-platform to reference the parent component.
CONF_LORAWAN_ID = "lorawan_id"

CONF_REGION = "region"
CONF_SUB_BAND = "sub_band"
CONF_DEV_EUI = "dev_eui"
CONF_JOIN_EUI = "join_eui"
CONF_APP_KEY = "app_key"
CONF_UPLINK_INTERVAL = "uplink_interval"
CONF_RADIO = "radio"
CONF_CHIP = "chip"
CONF_CS_PIN = "cs_pin"
CONF_RST_PIN = "rst_pin"
CONF_DIO0_PIN = "dio0_pin"
CONF_DIO1_PIN = "dio1_pin"
CONF_BUSY_PIN = "busy_pin"

# RadioLib module class names, keyed by the config value. The C++ side branches
# on this string to construct the right module.
RADIO_CHIPS = ["sx1276", "sx1278", "sx1262"]

# Spike scope is US915 sub-band 2 (the validated gateway). Other regions are
# accepted by the schema so the band table can grow without a schema change,
# but only US915 has been exercised.
REGIONS = ["US915", "EU868", "AU915", "AS923"]


def _hex_of_len(nibbles):
    def validator(value):
        value = cv.string_strict(value).strip().lower().replace(":", "")
        if len(value) != nibbles or any(c not in "0123456789abcdef" for c in value):
            raise cv.Invalid(f"expected {nibbles} hex digits, got {value!r}")
        return value

    return validator


RADIO_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_CHIP): cv.one_of(*RADIO_CHIPS, lower=True),
        cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_RST_PIN): pins.internal_gpio_output_pin_number,
        # SX1276/78 use dio0; SX1262 uses dio1 + busy. The C++ side validates the
        # combination against the chosen chip at setup; here both are optional.
        cv.Optional(CONF_DIO0_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_DIO1_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_number,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LoRaWANComponent),
        cv.Optional(CONF_REGION, default="US915"): cv.one_of(*REGIONS, upper=True),
        cv.Optional(CONF_SUB_BAND, default=2): cv.int_range(min=0, max=8),
        cv.Required(CONF_DEV_EUI): _hex_of_len(16),
        cv.Required(CONF_JOIN_EUI): _hex_of_len(16),
        cv.Required(CONF_APP_KEY): _hex_of_len(32),
        cv.Optional(
            CONF_UPLINK_INTERVAL, default="5min"
        ): cv.positive_time_period_milliseconds,
        cv.Required(CONF_RADIO): RADIO_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # Upstream RadioLib first; fall back to an esphome-compile fork only if this
    # will not build (see docs/esphome-component-pivot.md, open question).
    cg.add_library("jgromes/RadioLib", "7.2.1")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = config[CONF_RADIO]
    cg.add(var.set_chip(radio[CONF_CHIP]))
    cg.add(
        var.set_radio_pins(
            radio[CONF_CS_PIN],
            radio.get(CONF_DIO0_PIN, radio.get(CONF_DIO1_PIN, -1)),
            radio[CONF_RST_PIN],
            radio.get(CONF_BUSY_PIN, -1),
        )
    )
    cg.add(var.set_region(config[CONF_REGION]))
    cg.add(var.set_sub_band(config[CONF_SUB_BAND]))
    cg.add(var.set_uplink_interval(config[CONF_UPLINK_INTERVAL]))
    cg.add(var.set_credentials(config[CONF_JOIN_EUI], config[CONF_DEV_EUI], config[CONF_APP_KEY]))
