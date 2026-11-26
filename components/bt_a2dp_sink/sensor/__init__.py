import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_TYPE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_PERCENT,
)
from esphome.types import ConfigType

from .. import BT_A2DP_SINK_CHILD_SCHEMA, CONF_BT_A2DP_SINK_ID

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

CONF_PAIRED_DEVICES = "paired_devices"
CONF_BUFFER_FILL = "buffer_fill"
CONF_UNDERRUNS = "underruns"
CONF_OVERFLOWS = "overflows"

_SETTERS = {
    CONF_PAIRED_DEVICES: "set_paired_devices_sensor",
    CONF_BUFFER_FILL: "set_buffer_fill_sensor",
    CONF_UNDERRUNS: "set_underruns_sensor",
    CONF_OVERFLOWS: "set_overflows_sensor",
}

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_PAIRED_DEVICES: sensor.sensor_schema(
                icon="mdi:devices",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_BUFFER_FILL: sensor.sensor_schema(
                icon="mdi:tray-full",
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_UNDERRUNS: sensor.sensor_schema(
                icon="mdi:alert-circle-outline",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_OVERFLOWS: sensor.sensor_schema(
                icon="mdi:tray-alert",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
        },
        key=CONF_TYPE,
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BT_A2DP_SINK_ID])
    var = await sensor.new_sensor(config)
    cg.add(getattr(hub, _SETTERS[config[CONF_TYPE]])(var))
