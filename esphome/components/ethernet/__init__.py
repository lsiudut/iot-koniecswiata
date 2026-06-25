import logging

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components.network import ip_address_literal
from esphome.config_helpers import filter_source_files_from_platform
import esphome.config_validation as cv
from esphome.const import (
    CONF_CLK_PIN,
    CONF_CS_PIN,
    CONF_DNS1,
    CONF_DNS2,
    CONF_DOMAIN,
    CONF_GATEWAY,
    CONF_ID,
    CONF_INTERRUPT_PIN,
    CONF_MAC_ADDRESS,
    CONF_MANUAL_IP,
    CONF_MISO_PIN,
    CONF_MOSI_PIN,
    CONF_ON_CONNECT,
    CONF_ON_DISCONNECT,
    CONF_POLLING_INTERVAL,
    CONF_RESET_PIN,
    CONF_STATIC_IP,
    CONF_SUBNET,
    CONF_TYPE,
    CONF_USE_ADDRESS,
    Platform,
    PlatformFramework,
)
from esphome.core import CORE, CoroPriority, TimePeriodMilliseconds, coroutine_with_priority

CONFLICTS_WITH = ["wifi"]
AUTO_LOAD = ["network"]
LOGGER = logging.getLogger(__name__)

CONF_CLOCK_SPEED = "clock_speed"
CONF_INTERFACE = "interface"
CONF_NCOL_PIN = "ncol_pin"
CONF_NSPD_PIN = "nspd_pin"
CONF_NDPX_PIN = "ndpx_pin"

ethernet_ns = cg.esphome_ns.namespace("ethernet")
EthernetComponent = ethernet_ns.class_("EthernetComponent", cg.Component)
EthernetType = ethernet_ns.enum("EthernetType")
ManualIP = ethernet_ns.struct("ManualIP")

ETHERNET_TYPES = {
    "W6100": EthernetType.ETHERNET_TYPE_W6100,
}

spi_host_device_t = cg.global_ns.enum("spi_host_device_t")
SPI_INTERFACE_MAP = {
    "spi2": spi_host_device_t.SPI2_HOST,
    "spi3": spi_host_device_t.SPI3_HOST,
}

SPI_ETHERNET_DEFAULT_POLLING_INTERVAL = TimePeriodMilliseconds(milliseconds=10)

MANUAL_IP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_STATIC_IP): cv.ipv4address,
        cv.Required(CONF_GATEWAY): cv.ipv4address,
        cv.Required(CONF_SUBNET): cv.ipv4address,
        cv.Optional(CONF_DNS1, default="0.0.0.0"): cv.ipv4address,
        cv.Optional(CONF_DNS2, default="0.0.0.0"): cv.ipv4address,
    }
)


def _is_framework_spi_polling_mode_supported() -> bool:
    if not CORE.is_esp32:
        return False
    from esphome.components.esp32 import idf_version

    ver = idf_version()
    if ver >= cv.Version(5, 3, 0):
        return True
    if cv.Version(5, 3, 0) > ver >= cv.Version(5, 2, 1):
        return True
    if cv.Version(5, 2, 0) > ver >= cv.Version(5, 1, 4):
        return True
    return False


def _validate(config):
    if CONF_USE_ADDRESS not in config:
        if CONF_MANUAL_IP in config:
            use_address = str(config[CONF_MANUAL_IP][CONF_STATIC_IP])
        else:
            use_address = CORE.name + config[CONF_DOMAIN]
        config[CONF_USE_ADDRESS] = use_address

    if CONF_POLLING_INTERVAL in config and CONF_INTERRUPT_PIN in config:
        raise cv.Invalid(f"Cannot specify more than one of {CONF_INTERRUPT_PIN}, {CONF_POLLING_INTERVAL}")

    if _is_framework_spi_polling_mode_supported():
        if CONF_POLLING_INTERVAL not in config and CONF_INTERRUPT_PIN not in config:
            config[CONF_POLLING_INTERVAL] = SPI_ETHERNET_DEFAULT_POLLING_INTERVAL
    else:
        if CONF_POLLING_INTERVAL in config:
            raise cv.Invalid(f"{CONF_POLLING_INTERVAL} is not supported by this ESP-IDF version")
        if CONF_INTERRUPT_PIN not in config:
            raise cv.Invalid(f"{CONF_INTERRUPT_PIN} is required by this ESP-IDF version")

    return config


def _validate_spi_interface(config):
    if not CORE.is_esp32:
        return config
    from esphome.components.esp32 import VARIANT_ESP32, get_esp32_variant

    if CONF_INTERFACE not in config:
        config[CONF_INTERFACE] = "spi3" if get_esp32_variant() == VARIANT_ESP32 else "spi2"
    return config


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            "W6100": cv.Schema(
                {
                    cv.GenerateID(): cv.declare_id(EthernetComponent),
                    cv.Optional(CONF_MANUAL_IP): MANUAL_IP_SCHEMA,
                    cv.Optional(CONF_DOMAIN, default=".local"): cv.domain_name,
                    cv.Optional(CONF_USE_ADDRESS): cv.string_strict,
                    cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
                    cv.Optional(CONF_ON_CONNECT): automation.validate_automation(single=True),
                    cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation(single=True),
                    cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
                    cv.Required(CONF_MISO_PIN): pins.internal_gpio_input_pin_number,
                    cv.Required(CONF_MOSI_PIN): pins.internal_gpio_output_pin_number,
                    cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_number,
                    cv.Optional(CONF_INTERRUPT_PIN): pins.internal_gpio_input_pin_number,
                    cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
                    cv.Optional(CONF_NCOL_PIN): pins.internal_gpio_input_pin_number,
                    cv.Optional(CONF_NSPD_PIN): pins.internal_gpio_input_pin_number,
                    cv.Optional(CONF_NDPX_PIN): pins.internal_gpio_input_pin_number,
                    cv.Optional(CONF_INTERFACE): cv.enum(SPI_INTERFACE_MAP, lower=True),
                    cv.SplitDefault(CONF_CLOCK_SPEED, esp32="26.67MHz"): cv.All(
                        cv.only_on_esp32,
                        cv.frequency,
                        cv.int_range(int(8e6), int(80e6)),
                    ),
                    cv.Optional(CONF_POLLING_INTERVAL): cv.All(
                        cv.only_on_esp32,
                        cv.positive_time_period_milliseconds,
                        cv.Range(min=TimePeriodMilliseconds(milliseconds=1)),
                    ),
                }
            ).extend(cv.COMPONENT_SCHEMA),
        },
        upper=True,
    ),
    cv.only_on([Platform.ESP32]),
    _validate_spi_interface,
    _validate,
)


def manual_ip(config):
    return cg.StructInitializer(
        ManualIP,
        ("static_ip", ip_address_literal(config[CONF_STATIC_IP])),
        ("gateway", ip_address_literal(config[CONF_GATEWAY])),
        ("subnet", ip_address_literal(config[CONF_SUBNET])),
        ("dns1", ip_address_literal(config[CONF_DNS1])),
        ("dns2", ip_address_literal(config[CONF_DNS2])),
    )


@coroutine_with_priority(CoroPriority.COMMUNICATION)
async def to_code(config):
    from esphome.components.esp32 import add_idf_sdkconfig_option, include_builtin_idf_component

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_type(ETHERNET_TYPES[config[CONF_TYPE]]))
    cg.add(var.set_use_address(config[CONF_USE_ADDRESS]))
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_miso_pin(config[CONF_MISO_PIN]))
    cg.add(var.set_mosi_pin(config[CONF_MOSI_PIN]))
    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_interface(SPI_INTERFACE_MAP[config[CONF_INTERFACE]]))
    cg.add(var.set_clock_speed(config[CONF_CLOCK_SPEED]))

    if CONF_INTERRUPT_PIN in config:
        cg.add(var.set_interrupt_pin(config[CONF_INTERRUPT_PIN]))
    else:
        cg.add(var.set_polling_interval(config[CONF_POLLING_INTERVAL]))
        cg.add_define("USE_ETHERNET_SPI_POLLING_SUPPORT")

    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    if CONF_NCOL_PIN in config:
        cg.add(var.set_ncol_pin(config[CONF_NCOL_PIN]))
    if CONF_NSPD_PIN in config:
        cg.add(var.set_nspd_pin(config[CONF_NSPD_PIN]))
    if CONF_NDPX_PIN in config:
        cg.add(var.set_ndpx_pin(config[CONF_NDPX_PIN]))

    if CONF_MANUAL_IP in config:
        cg.add_define("USE_ETHERNET_MANUAL_IP")
        cg.add(var.set_manual_ip(manual_ip(config[CONF_MANUAL_IP])))

    if mac_address := config.get(CONF_MAC_ADDRESS):
        cg.add(var.set_fixed_mac(mac_address.parts))

    if on_connect_config := config.get(CONF_ON_CONNECT):
        cg.add_define("USE_ETHERNET_CONNECT_TRIGGER")
        await automation.build_automation(var.get_connect_trigger(), [], on_connect_config)

    if on_disconnect_config := config.get(CONF_ON_DISCONNECT):
        cg.add_define("USE_ETHERNET_DISCONNECT_TRIGGER")
        await automation.build_automation(var.get_disconnect_trigger(), [], on_disconnect_config)

    cg.add_define("USE_ETHERNET")
    cg.add_define("USE_ETHERNET_SPI")
    cg.add_define("USE_ETHERNET_W6100")
    add_idf_sdkconfig_option("CONFIG_ETH_USE_SPI_ETHERNET", True)
    include_builtin_idf_component("esp_eth")


FILTER_SOURCE_FILES = filter_source_files_from_platform(
    {
        "ethernet_component_esp32.cpp": {
            PlatformFramework.ESP32_IDF,
            PlatformFramework.ESP32_ARDUINO,
        },
        "esp_eth_mac_w6100.c": {
            PlatformFramework.ESP32_IDF,
            PlatformFramework.ESP32_ARDUINO,
        },
        "esp_eth_phy_w6100.c": {
            PlatformFramework.ESP32_IDF,
            PlatformFramework.ESP32_ARDUINO,
        },
    }
)
