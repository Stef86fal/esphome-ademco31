import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from . import (
    Ademco4110Component,
    CONF_TYPE,
    SYSTEM_SENSOR_TYPES,
)

DEPENDENCIES = ["ademco4110"]

CONF_ADEMCO_ID = "ademco4110_id"

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend({
    cv.GenerateID(CONF_ADEMCO_ID): cv.use_id(Ademco4110Component),
    cv.Optional(CONF_TYPE): cv.one_of(*SYSTEM_SENSOR_TYPES, lower=True),
})


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    panel = await cg.get_variable(config[CONF_ADEMCO_ID])
    if CONF_TYPE in config:
        cg.add(getattr(panel, SYSTEM_SENSOR_TYPES[config[CONF_TYPE]])(var))
