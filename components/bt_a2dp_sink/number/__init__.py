import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, UNIT_PERCENT
from esphome.types import ConfigType

from .. import BT_A2DP_SINK_CHILD_SCHEMA, CONF_BT_A2DP_SINK_ID, bt_a2dp_sink_ns

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

CONF_PHONE_VOLUME = "phone_volume"

BtPhoneVolumeNumber = bt_a2dp_sink_ns.class_("BtPhoneVolumeNumber", number.Number)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_PHONE_VOLUME: number.number_schema(
                BtPhoneVolumeNumber,
                icon="mdi:volume-high",
                unit_of_measurement=UNIT_PERCENT,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
        },
        key=CONF_TYPE,
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BT_A2DP_SINK_ID])
    var = await number.new_number(config, min_value=0.0, max_value=100.0, step=1.0)
    await cg.register_parented(var, hub)
    cg.add(hub.set_phone_volume_number(var))
