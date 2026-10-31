"""Mealie: the ESPHome side. The values the top bar's item needs are ESPHome entities of plugin.yaml; this component
reads them and registers the tile, the card and the bar item (mealie.cpp). smart_display.register_plugin() takes the
id, version, tile memory and the screen's texts from the manifest and translations/ beside this folder
(docs/FIRMWARE_API.md of tessera-plugins)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display, text_sensor
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display"]

CONF_MESSAGE = "message"
CONF_START = "start"
CONF_MEAL = "meal"

mealie_ns = cg.esphome_ns.namespace("mealie")
Mealie = mealie_ns.class_("Mealie", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Mealie),
        cv.Required(CONF_MESSAGE): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_START): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_MEAL): cv.entity_id,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_message(await cg.get_variable(config[CONF_MESSAGE])))
    cg.add(var.set_start(await cg.get_variable(config[CONF_START])))
    cg.add(var.set_meal(config[CONF_MEAL]))
