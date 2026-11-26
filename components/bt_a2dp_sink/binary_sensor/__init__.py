import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_RUNNING,
)
from esphome.types import ConfigType

from .. import BT_A2DP_SINK_CHILD_SCHEMA, CONF_BT_A2DP_SINK_ID

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

CONF_CONNECTED = "connected"
CONF_STREAMING = "streaming"
CONF_PAIRING_MODE = "pairing_mode"

_SETTERS = {
    CONF_CONNECTED: "set_connected_binary_sensor",
    CONF_STREAMING: "set_streaming_binary_sensor",
    CONF_PAIRING_MODE: "set_pairing_mode_binary_sensor",
}

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_CONNECTED: binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                icon="mdi:bluetooth-connect",
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_STREAMING: binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_RUNNING,
                icon="mdi:waveform",
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_PAIRING_MODE: binary_sensor.binary_sensor_schema(
                icon="mdi:bluetooth-audio",
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
        },
        key=CONF_TYPE,
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BT_A2DP_SINK_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(getattr(hub, _SETTERS[config[CONF_TYPE]])(var))
