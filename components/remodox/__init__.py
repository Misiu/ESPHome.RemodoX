import esphome.codegen as cg
from esphome.core import CORE
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID


DEPENDENCIES = ['ble_client']
AUTO_LOAD = ["sensor", "text_sensor"]
CODEOWNERS = ["@Misiu"]

MULTI_CONF=3

remodox_ns = cg.esphome_ns.namespace("remodox")

RemodoX = remodox_ns.class_(
    "RemodoX",
    cg.Component,
    ble_client.BLEClientNode,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RemodoX)
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)

CONF_REMODOX_ID = "remodox_id"

REMODOX_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_REMODOX_ID): cv.use_id(RemodoX),
    }
)

async def register_last_event_usage_text_sensor(var, config):
    parent = await cg.get_variable(config[CONF_REMODOX_ID])
    cg.add(parent.register_last_event_usage_text_sensor(var))

async def register_last_event_code_text_sensor(var, config):
    parent = await cg.get_variable(config[CONF_REMODOX_ID])
    cg.add(parent.register_last_event_code_text_sensor(var))

async def register_last_event_value_sensor(var, config):
    parent = await cg.get_variable(config[CONF_REMODOX_ID])
    cg.add(parent.register_last_event_value_sensor(var))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
