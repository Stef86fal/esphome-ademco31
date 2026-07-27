import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from . import Ademco4110Component

DEPENDENCIES = ["ademco4110"]

CONF_ADEMCO_ID = "ademco4110_id"
CONF_TYPE      = "type"

TEXT_SENSOR_TYPES = {
    "status": "set_status_sensor",
    "zones":  "set_zones_sensor",
    "alarm_zone": "set_alarm_zone_sensor",
    "zone_changed": "set_zone_changed_sensor",
}

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend({
    cv.GenerateID(CONF_ADEMCO_ID): cv.use_id(Ademco4110Component),
    cv.Optional(CONF_TYPE, default="status"): cv.one_of(*TEXT_SENSOR_TYPES, lower=True),
})


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    panel = await cg.get_variable(config[CONF_ADEMCO_ID])
    cg.add(getattr(panel, TEXT_SENSOR_TYPES[config[CONF_TYPE]])(var))
