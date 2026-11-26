import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from .. import BT_A2DP_SINK_CHILD_SCHEMA, CONF_BT_A2DP_SINK_ID

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

CONF_DEVICE_NAME = "device_name"
CONF_DEVICE_ADDRESS = "device_address"
CONF_CODEC = "codec"

_SETTERS = {
    CONF_DEVICE_NAME: "set_device_name_text_sensor",
    CONF_DEVICE_ADDRESS: "set_device_address_text_sensor",
    CONF_CODEC: "set_codec_text_sensor",
}

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_DEVICE_NAME: text_sensor.text_sensor_schema(
                icon="mdi:speaker-bluetooth",
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_DEVICE_ADDRESS: text_sensor.text_sensor_schema(
                icon="mdi:identifier",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_CODEC: text_sensor.text_sensor_schema(
                icon="mdi:sine-wave",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
        },
        key=CONF_TYPE,
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BT_A2DP_SINK_ID])
    var = await text_sensor.new_text_sensor(config)
    cg.add(getattr(hub, _SETTERS[config[CONF_TYPE]])(var))
