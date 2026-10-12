import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NUMBER, CONF_RX_PIN
from esphome.core import CORE

DEPENDENCIES = ["hcpbridge"]
CODEOWNERS = ["@lsiudut"]

CONF_LOG_FRAMES = "log_frames"
CONF_REPORT_INTERVAL = "report_interval"

hcpbridge_debug_ns = cg.esphome_ns.namespace("hcpbridge_debug")
HCPBridgeDebug = hcpbridge_debug_ns.class_("HCPBridgeDebug", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HCPBridgeDebug),
            cv.Optional(CONF_LOG_FRAMES, default=True): cv.boolean,
            cv.Optional(
                CONF_REPORT_INTERVAL, default="10s"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_arduino,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # The hcpbridge RX pin feeds a hardware edge counter and idle-level
    # sampling. The UART keeps ownership of the pin.
    rx_pin = CORE.config.get("hcpbridge", {}).get(CONF_RX_PIN)
    if rx_pin is not None:
        cg.add(var.set_rx_pin(rx_pin[CONF_NUMBER]))
    cg.add(var.set_log_frames(config[CONF_LOG_FRAMES]))
    cg.add(var.set_report_interval(config[CONF_REPORT_INTERVAL]))
