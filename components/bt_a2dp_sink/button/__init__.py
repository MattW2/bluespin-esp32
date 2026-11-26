import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE, ENTITY_CATEGORY_CONFIG
from esphome.types import ConfigType

from .. import BT_A2DP_SINK_CHILD_SCHEMA, CONF_BT_A2DP_SINK_ID, bt_a2dp_sink_ns

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

CONF_START_PAIRING = "start_pairing"
CONF_STOP_PAIRING = "stop_pairing"
CONF_DISCONNECT = "disconnect"
CONF_RECONNECT_LAST = "reconnect_last"
CONF_FORGET_ALL = "forget_all"

BtA2dpButton = bt_a2dp_sink_ns.class_("BtA2dpButton", button.Button)
BtButtonType = bt_a2dp_sink_ns.enum("BtButtonType", is_class=True)

_BUTTON_TYPES = {
    CONF_START_PAIRING: BtButtonType.START_PAIRING,
    CONF_STOP_PAIRING: BtButtonType.STOP_PAIRING,
    CONF_DISCONNECT: BtButtonType.DISCONNECT,
    CONF_RECONNECT_LAST: BtButtonType.RECONNECT_LAST,
    CONF_FORGET_ALL: BtButtonType.FORGET_ALL,
}

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_START_PAIRING: button.button_schema(
                BtA2dpButton, icon="mdi:bluetooth-connect"
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_STOP_PAIRING: button.button_schema(
                BtA2dpButton, icon="mdi:bluetooth-off"
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_DISCONNECT: button.button_schema(
                BtA2dpButton, icon="mdi:link-off"
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_RECONNECT_LAST: button.button_schema(
                BtA2dpButton, icon="mdi:bluetooth-transfer"
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_FORGET_ALL: button.button_schema(
                BtA2dpButton,
                icon="mdi:delete-forever",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
        },
        key=CONF_TYPE,
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    var = await button.new_button(config)
    await cg.register_parented(var, config[CONF_BT_A2DP_SINK_ID])
    cg.add(var.set_button_type(_BUTTON_TYPES[config[CONF_TYPE]]))
