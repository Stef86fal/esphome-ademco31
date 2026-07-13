import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins, automation
from esphome.components import uart
from esphome.const import CONF_ID, CONF_UART_ID

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "text_sensor"]

ademco4110_ns = cg.esphome_ns.namespace("ademco4110")
Ademco4110Component = ademco4110_ns.class_(
    "Ademco4110Component", cg.Component, uart.UARTDevice
)
SendKeysAction = ademco4110_ns.class_("SendKeysAction", automation.Action)
ScanZonesAction = ademco4110_ns.class_("ScanZonesAction", automation.Action)

CONF_SYNC_PIN    = "sync_pin"
CONF_KEYS        = "keys"
CONF_DIAGNOSTIC  = "diagnostic_mode"
CONF_TYPE        = "type"
CONF_DISARM_CODE = "disarm_code"

SYSTEM_SENSOR_TYPES = {
    "armed":         "set_armed_sensor",
    "armed_total":   "set_armed_total_sensor",
    "armed_partial": "set_armed_partial_sensor",
    "alarm":         "set_alarm_sensor",
    "chime":         "set_chime_sensor",
    "ready":         "set_ready_sensor",
    "ac_power":      "set_ac_power_sensor",
    "battery_low":   "set_battery_low_sensor",
    "bypass":        "set_bypass_sensor",
}

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Ademco4110Component),
    cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_SYNC_PIN): pins.internal_gpio_input_pin_schema,
    cv.Optional(CONF_DIAGNOSTIC, default=False): cv.boolean,
    cv.Optional(CONF_DISARM_CODE, default=""): cv.string,
}).extend(cv.COMPONENT_SCHEMA)

SEND_KEYS_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(Ademco4110Component),
    cv.Required(CONF_KEYS): cv.templatable(cv.string),
})

SCAN_ZONES_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(Ademco4110Component),
})

@automation.register_action("ademco4110.send_keys", SendKeysAction, SEND_KEYS_ACTION_SCHEMA)
async def send_keys_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    keys_ = await cg.templatable(config[CONF_KEYS], args, cg.std_string)
    cg.add(var.set_keys(keys_))
    return var

@automation.register_action("ademco4110.scan_zones", ScanZonesAction, SCAN_ZONES_ACTION_SCHEMA)
async def scan_zones_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_SYNC_PIN in config:
        sync = await cg.gpio_pin_expression(config[CONF_SYNC_PIN])
        cg.add(var.set_sync_pin(sync))
    cg.add(var.set_diagnostic_mode(config[CONF_DIAGNOSTIC]))
    cg.add(var.set_disarm_code(config[CONF_DISARM_CODE]))
