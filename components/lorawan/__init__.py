import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@moellere"]
# sensor bindings are an optional sub-platform; the component works without any.
AUTO_LOAD = ["sensor"]
MULTI_CONF = False

lorawan_ns = cg.esphome_ns.namespace("lorawan")
LoRaWANComponent = lorawan_ns.class_("LoRaWANComponent", cg.Component)
DownlinkTrigger = lorawan_ns.class_(
    "DownlinkTrigger",
    automation.Trigger.template(cg.uint8, cg.std_vector.template(cg.uint8)),
)

SendRawAction = lorawan_ns.class_("SendRawAction", automation.Action)

# Used by the sensor sub-platform to reference the parent component.
CONF_LORAWAN_ID = "lorawan_id"
CONF_ON_DOWNLINK = "on_downlink"
CONF_DEVICE_CLASS = "device_class"
CONF_F_PORT = "f_port"
CONF_PAYLOAD = "payload"

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
CONF_SCK_PIN = "sck_pin"
CONF_MISO_PIN = "miso_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_TCXO_VOLTAGE = "tcxo_voltage"
CONF_DIO2_AS_RF_SWITCH = "dio2_as_rf_switch"
CONF_SETUP_HIGH = "setup_high"
CONF_SETUP_LOW = "setup_low"
CONF_RXEN_PIN = "rxen_pin"
CONF_TXEN_PIN = "txen_pin"

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


RADIO_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_CHIP): cv.one_of(*RADIO_CHIPS, lower=True),
            cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_RST_PIN): pins.internal_gpio_output_pin_number,
            # SX1276/78 use dio0; SX1262 uses dio1 + busy. The C++ side validates
            # the combination against the chosen chip at setup; both optional here.
            cv.Optional(CONF_DIO0_PIN): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_DIO1_PIN): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_number,
            # Explicit SPI bus pins. RadioLib otherwise defaults to the ESP32 VSPI
            # pins (18/19/23/5), which match almost no LoRa board's wiring and
            # surface as ERR_CHIP_NOT_FOUND. All three or none -- SPI.begin()
            # needs the full set.
            cv.Optional(CONF_SCK_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_MISO_PIN): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_MOSI_PIN): pins.internal_gpio_output_pin_number,
            # SX1262 boards clock the radio from a TCXO the SX1262 powers
            # itself over DIO3. RadioLib has to be told the voltage or the
            # oscillator never starts and begin() fails ERR_SPI_CMD_TIMEOUT --
            # which reads like miswired SPI. 0 means "crystal, not TCXO".
            cv.Optional(CONF_TCXO_VOLTAGE): cv.All(
                cv.float_range(min=0.0, max=3.3), cv.float_
            ),
            # Many SX1262 modules wire the antenna switch to DIO2 instead of a
            # GPIO. Without this the radio transmits into a switch that never
            # flips: the device reports sending and nothing leaves the antenna.
            cv.Optional(CONF_DIO2_AS_RF_SWITCH, default=False): cv.boolean,
            # Pins that must be driven high before the radio is touched --
            # front-end module power/enable, PA mode selects. Boards with an
            # external PA (Heltec V3/V4) are deaf until these are asserted, and
            # the failure is silent: begin() succeeds, joins never arrive.
            cv.Optional(CONF_SETUP_HIGH): cv.ensure_list(
                pins.internal_gpio_output_pin_number
            ),
            cv.Optional(CONF_SETUP_LOW): cv.ensure_list(
                pins.internal_gpio_output_pin_number
            ),
            # PA/LNA enables RadioLib toggles per transfer (idle LOW, txen
            # HIGH during TX, rxen HIGH during RX). For enables that must NOT
            # be held statically: Heltec V4.2's GC1109 PA_TX_EN (GPIO46)
            # pinned high leaves the PA engaged and the receiver deaf.
            cv.Optional(CONF_RXEN_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_TXEN_PIN): pins.internal_gpio_output_pin_number,
        }
    ),
    cv.has_none_or_all_keys(CONF_SCK_PIN, CONF_MISO_PIN, CONF_MOSI_PIN),
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
        # Class C keeps the receiver open between uplinks (mains/large-battery
        # devices only): downlinks land in seconds instead of at the next
        # uplink's RX window. Class B (beaconing) is not supported by RadioLib.
        cv.Optional(CONF_DEVICE_CLASS, default="A"): cv.one_of("A", "C", upper=True),
        cv.Required(CONF_RADIO): RADIO_SCHEMA,
        cv.Optional(CONF_ON_DOWNLINK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DownlinkTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # Upstream RadioLib first; fall back to an esphome-compile fork only if this
    # will not build (see docs/esphome-component-pivot.md, open question).
    cg.add_library("jgromes/RadioLib", "7.2.1")
    # RadioLib's Module.h includes Arduino <SPI.h>. ESPHome's own spi component
    # skips that library on ESP32 (it uses the IDF SPI driver), so pull it in
    # explicitly or RadioLib won't find SPI.h.
    cg.add_library("SPI", None)

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
    if CONF_SCK_PIN in radio:
        cg.add(var.set_sck_pin(radio[CONF_SCK_PIN]))
        cg.add(var.set_miso_pin(radio[CONF_MISO_PIN]))
        cg.add(var.set_mosi_pin(radio[CONF_MOSI_PIN]))
    if CONF_TCXO_VOLTAGE in radio:
        cg.add(var.set_tcxo_voltage(radio[CONF_TCXO_VOLTAGE]))
    cg.add(var.set_dio2_as_rf_switch(radio[CONF_DIO2_AS_RF_SWITCH]))
    for pin in radio.get(CONF_SETUP_HIGH, []):
        cg.add(var.add_setup_high_pin(pin))
    for pin in radio.get(CONF_SETUP_LOW, []):
        cg.add(var.add_setup_low_pin(pin))
    if CONF_RXEN_PIN in radio or CONF_TXEN_PIN in radio:
        cg.add(var.set_rf_switch_pins(radio.get(CONF_RXEN_PIN, -1), radio.get(CONF_TXEN_PIN, -1)))
    cg.add(var.set_region(config[CONF_REGION]))
    cg.add(var.set_sub_band(config[CONF_SUB_BAND]))
    cg.add(var.set_uplink_interval(config[CONF_UPLINK_INTERVAL]))
    cg.add(var.set_device_class(config[CONF_DEVICE_CLASS]))
    cg.add(var.set_credentials(config[CONF_JOIN_EUI], config[CONF_DEV_EUI], config[CONF_APP_KEY]))

    for conf in config.get(CONF_ON_DOWNLINK, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_downlink_trigger(trigger))
        await automation.build_automation(
            trigger,
            [(cg.uint8, "port"), (cg.std_vector.template(cg.uint8), "payload")],
            conf,
        )


SEND_RAW_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(LoRaWANComponent),
        # fPort 0 is MAC-only and 224+ is reserved for test; application data
        # lives in 1..223.
        cv.Optional(CONF_F_PORT, default=1): cv.templatable(cv.int_range(min=1, max=223)),
        # A literal list of bytes, or a lambda returning std::vector<uint8_t>
        # (the usual case: pack binary telemetry on the fly).
        cv.Required(CONF_PAYLOAD): cv.templatable(cv.ensure_list(cv.hex_uint8_t)),
    }
)


@automation.register_action("lorawan.send_raw", SendRawAction, SEND_RAW_SCHEMA)
async def send_raw_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(
        config[CONF_F_PORT], args, cg.uint8
    )
    cg.add(var.set_f_port(template_))
    template_ = await cg.templatable(
        config[CONF_PAYLOAD], args, cg.std_vector.template(cg.uint8)
    )
    cg.add(var.set_payload(template_))
    return var
