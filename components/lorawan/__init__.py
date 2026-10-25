import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components import deep_sleep, spi
from esphome.const import (
    CONF_CS_PIN,
    CONF_ID,
    CONF_RUN_DURATION,
    CONF_SLEEP_DURATION,
    CONF_SPI_ID,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE
from esphome.final_validate import full_config

CODEOWNERS = ["@moellere"]
DEPENDENCIES = ["spi"]
# sensor bindings are an optional sub-platform; the component works without any.
AUTO_LOAD = ["sensor"]
MULTI_CONF = False

lorawan_ns = cg.esphome_ns.namespace("lorawan")
LoRaWANComponent = lorawan_ns.class_("LoRaWANComponent", cg.Component, spi.SPIDevice)
DownlinkTrigger = lorawan_ns.class_(
    "DownlinkTrigger",
    automation.Trigger.template(cg.uint8, cg.std_vector.template(cg.uint8)),
)

SendRawAction = lorawan_ns.class_("SendRawAction", automation.Action)

# Used by the sensor sub-platform to reference the parent component.
CONF_LORAWAN_ID = "lorawan_id"
CONF_ON_DOWNLINK = "on_downlink"
CONF_DEVICE_CLASS = "device_class"
CONF_DEEP_SLEEP_ID = "deep_sleep_id"
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

_LOGGER = logging.getLogger(__name__)

# Bands wired in init_radio_(). Only US915 (sub-band 2) has been run on
# hardware; the others are RadioLib's own band tables, compile-checked only.
REGIONS = ["US915", "EU868", "AU915"]
UNTESTED_REGIONS = ["EU868", "AU915"]
# Fixed-channel plans select an 8-channel group with sub_band; EU868's channels
# come from the network instead.
SUB_BAND_REGIONS = ["US915", "AU915"]


def _hex_of_len(nibbles):
    def validator(value):
        value = cv.string_strict(value).strip().lower().replace(":", "")
        if len(value) != nibbles or any(c not in "0123456789abcdef" for c in value):
            raise cv.Invalid(f"expected {nibbles} hex digits, got {value!r}")
        return value

    return validator


_MOVED_TO_SPI = (
    "moved to a top-level `spi:` block (clk_pin, miso_pin, mosi_pin); the radio "
    "is an ESPHome SPI device and keeps only cs_pin"
)

def _validate_irq_pins(radio):
    # RadioLib's LoRaWAN MAC waits on the radio's interrupt line.
    if radio[CONF_CHIP] == "sx1262":
        for key in (CONF_DIO1_PIN, CONF_BUSY_PIN):
            if key not in radio:
                raise cv.Invalid(f"sx1262 needs {key}", path=[key])
    elif CONF_DIO0_PIN not in radio:
        raise cv.Invalid(f"{radio[CONF_CHIP]} needs {CONF_DIO0_PIN}", path=[CONF_DIO0_PIN])
    return radio


RADIO_SCHEMA = cv.All(cv.Schema(
    {
        cv.Required(CONF_CHIP): cv.one_of(*RADIO_CHIPS, lower=True),
        cv.Required(CONF_RST_PIN): pins.gpio_output_pin_schema,
        # SX1276/78 interrupt on dio0; SX1262 on dio1, plus busy.
        cv.Optional(CONF_DIO0_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_DIO1_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_BUSY_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_SCK_PIN): cv.invalid(_MOVED_TO_SPI),
        cv.Optional(CONF_MISO_PIN): cv.invalid(_MOVED_TO_SPI),
        cv.Optional(CONF_MOSI_PIN): cv.invalid(_MOVED_TO_SPI),
        # SX1262 boards clock the radio from a TCXO the SX1262 powers
        # itself over DIO3. RadioLib has to be told the voltage or the
        # oscillator never starts and begin() fails ERR_SPI_CMD_TIMEOUT --
        # which reads like miswired SPI. 0 means "crystal, not TCXO".
        cv.Optional(CONF_TCXO_VOLTAGE): cv.All(cv.float_range(min=0.0, max=3.3), cv.float_),
        # Many SX1262 modules wire the antenna switch to DIO2 instead of a
        # GPIO. Without this the radio transmits into a switch that never
        # flips: the device reports sending and nothing leaves the antenna.
        cv.Optional(CONF_DIO2_AS_RF_SWITCH, default=False): cv.boolean,
        # Pins that must be driven high (or low) before the radio is touched --
        # front-end module power/enable, PA mode selects. Boards with an
        # external PA (Heltec V3/V4) are deaf until these are asserted, and
        # the failure is silent: begin() succeeds, joins never arrive.
        cv.Optional(CONF_SETUP_HIGH): cv.ensure_list(pins.gpio_output_pin_schema),
        cv.Optional(CONF_SETUP_LOW): cv.ensure_list(pins.gpio_output_pin_schema),
        # Front-end selects RadioLib toggles per transfer (idle LOW, txen HIGH
        # during TX, rxen HIGH during RX), e.g. the Heltec V4's GC1109 CPS
        # (full PA vs bypass) on GPIO46.
        cv.Optional(CONF_RXEN_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_TXEN_PIN): pins.gpio_output_pin_schema,
    }
).extend(spi.spi_device_schema(cs_pin_required=True, default_data_rate=8e6, default_mode="MODE0")),
    _validate_irq_pins)


def _validate_region(config):
    region = config[CONF_REGION]
    if region in SUB_BAND_REGIONS:
        config.setdefault(CONF_SUB_BAND, 2)
    elif CONF_SUB_BAND in config:
        raise cv.Invalid(f"{region} has no sub-bands; remove sub_band", path=[CONF_SUB_BAND])
    else:
        config[CONF_SUB_BAND] = 0
    if region in UNTESTED_REGIONS:
        _LOGGER.warning(
            "lorawan region %s has never been run on hardware with this "
            "component; only US915 has", region
        )
    return config


def _validate_deep_sleep_class(config):
    # Class C exists to keep the receiver open between uplinks; asleep, there
    # is no receiver.
    if CONF_DEEP_SLEEP_ID in config and config[CONF_DEVICE_CLASS] == "C":
        raise cv.Invalid("device_class C cannot be combined with deep_sleep_id")
    return config


# ESP32 only: the radio task is a FreeRTOS task pinned to the loop's core.
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LoRaWANComponent),
            cv.Optional(CONF_REGION, default="US915"): cv.one_of(*REGIONS, upper=True),
            cv.Optional(CONF_SUB_BAND): cv.int_range(min=0, max=8),
            cv.Required(CONF_DEV_EUI): _hex_of_len(16),
            cv.Required(CONF_JOIN_EUI): _hex_of_len(16),
            cv.Required(CONF_APP_KEY): cv.sensitive(_hex_of_len(32)),
            cv.Optional(
                CONF_UPLINK_INTERVAL, default="5min"
            ): cv.positive_time_period_milliseconds,
            # Class C keeps the receiver open between uplinks (mains/large-battery
            # devices only): downlinks land in seconds instead of at the next
            # uplink's RX window. Class B (beaconing) is not supported by RadioLib.
            cv.Optional(CONF_DEVICE_CLASS, default="A"): cv.one_of("A", "C", upper=True),
            cv.Required(CONF_RADIO): RADIO_SCHEMA,
            # One uplink per wake: join or resume the stored session, wait for
            # the bound sensors, uplink, then hand over to this deep_sleep.
            # Its sleep_duration sets the uplink cadence; uplink_interval only
            # applies when this is not set.
            cv.Optional(CONF_DEEP_SLEEP_ID): cv.use_id(deep_sleep.DeepSleepComponent),
            cv.Optional(CONF_ON_DOWNLINK): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DownlinkTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_region,
    _validate_deep_sleep_class,
)


_SPI_BUS_SCHEMA = spi.final_validate_device_schema("lorawan", require_mosi=True, require_miso=True)


def _spi_users(node, spi_id, found):
    # Every config dict anywhere in the YAML that sits on this SPI bus.
    if isinstance(node, dict):
        if CONF_SPI_ID in node and str(node[CONF_SPI_ID]) == spi_id:
            found.append(node)
        for value in node.values():
            _spi_users(value, spi_id, found)
    elif isinstance(node, list):
        for value in node:
            _spi_users(value, spi_id, found)


def _final_validate(config):
    radio = config[CONF_RADIO]
    _SPI_BUS_SCHEMA(radio)
    # The radio task drives the bus concurrently with the main loop, and
    # ESPHome's SPI is not thread-safe, so nothing else may share it.
    users = []
    _spi_users(dict(full_config.get()), str(radio[CONF_SPI_ID]), users)
    if len(users) > 1:
        raise cv.Invalid(
            "the lorawan radio needs an SPI bus of its own: its radio task uses "
            "the bus concurrently with the main loop. Give the other device(s) "
            "a separate spi: bus.",
            path=[CONF_RADIO, CONF_SPI_ID],
        )
    if CONF_DEEP_SLEEP_ID not in config:
        return config
    fconf = full_config.get()
    ds = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_DEEP_SLEEP_ID])[:-1])
    if CONF_RUN_DURATION in ds:
        raise cv.Invalid(
            "deep_sleep run_duration must not be set when lorawan drives sleep: "
            "it could put the device to sleep in the middle of a join or uplink",
            path=[CONF_DEEP_SLEEP_ID],
        )
    if CONF_SLEEP_DURATION not in ds:
        raise cv.Invalid(
            "deep_sleep needs a sleep_duration when lorawan drives sleep: it is "
            "what schedules the next uplink",
            path=[CONF_DEEP_SLEEP_ID],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # Upstream RadioLib, built against the HAL in esphome_hal.cpp (ESPHome SPI
    # and GPIO), so it works under ESP-IDF as well as Arduino.
    cg.add_library("jgromes/RadioLib", "7.7.1")
    if CORE.using_arduino:
        # Under Arduino, RadioLib's Module.h still includes <SPI.h> (for its
        # unused ArduinoHal), and ESPHome's spi component does not pull in the
        # Arduino SPI library on ESP32.
        cg.add_library("SPI", None)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = config[CONF_RADIO]
    await spi.register_spi_device(var, radio)
    cg.add(var.set_chip(radio[CONF_CHIP]))
    cg.add(var.set_rst_pin(await cg.gpio_pin_expression(radio[CONF_RST_PIN])))
    irq = radio.get(CONF_DIO0_PIN, radio.get(CONF_DIO1_PIN))
    if irq is not None:
        cg.add(var.set_irq_pin(await cg.gpio_pin_expression(irq)))
    if CONF_BUSY_PIN in radio:
        cg.add(var.set_busy_pin(await cg.gpio_pin_expression(radio[CONF_BUSY_PIN])))
    if CONF_TCXO_VOLTAGE in radio:
        cg.add(var.set_tcxo_voltage(radio[CONF_TCXO_VOLTAGE]))
    cg.add(var.set_dio2_as_rf_switch(radio[CONF_DIO2_AS_RF_SWITCH]))
    for pin in radio.get(CONF_SETUP_HIGH, []):
        cg.add(var.add_setup_high_pin(await cg.gpio_pin_expression(pin)))
    for pin in radio.get(CONF_SETUP_LOW, []):
        cg.add(var.add_setup_low_pin(await cg.gpio_pin_expression(pin)))
    if CONF_RXEN_PIN in radio:
        cg.add(var.set_rxen_pin(await cg.gpio_pin_expression(radio[CONF_RXEN_PIN])))
    if CONF_TXEN_PIN in radio:
        cg.add(var.set_txen_pin(await cg.gpio_pin_expression(radio[CONF_TXEN_PIN])))
    cg.add(var.set_region(config[CONF_REGION]))
    cg.add(var.set_sub_band(config[CONF_SUB_BAND]))
    cg.add(var.set_uplink_interval(config[CONF_UPLINK_INTERVAL]))
    cg.add(var.set_device_class(config[CONF_DEVICE_CLASS]))
    cg.add(var.set_credentials(config[CONF_JOIN_EUI], config[CONF_DEV_EUI], config[CONF_APP_KEY]))
    if CONF_DEEP_SLEEP_ID in config:
        ds = await cg.get_variable(config[CONF_DEEP_SLEEP_ID])
        cg.add(var.set_deep_sleep(ds))

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


@automation.register_action(
    "lorawan.send_raw", SendRawAction, SEND_RAW_SCHEMA, synchronous=True
)
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
