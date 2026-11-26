import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG
from esphome.types import ConfigType

from .. import (
    BT_A2DP_SINK_CHILD_SCHEMA,
    CONF_ALLOW_DEVICE_CONNECT,
    CONF_BT_A2DP_SINK_ID,
    bt_a2dp_sink_ns,
)

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

CONF_PAIRING_MODE = "pairing_mode"

BtPairingModeSwitch = bt_a2dp_sink_ns.class_("BtPairingModeSwitch", switch.Switch)
BtAllowConnectSwitch = bt_a2dp_sink_ns.class_(
    "BtAllowConnectSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_PAIRING_MODE: switch.switch_schema(
                BtPairingModeSwitch,
                icon="mdi:bluetooth-audio",
                block_inverted=True,
                default_restore_mode="DISABLED",
            ).extend(BT_A2DP_SINK_CHILD_SCHEMA),
            CONF_ALLOW_DEVICE_CONNECT: switch.switch_schema(
                BtAllowConnectSwitch,
                icon="mdi:bluetooth-settings",
                block_inverted=True,
                default_restore_mode="RESTORE_DEFAULT_ON",
                entity_category=ENTITY_CATEGORY_CONFIG,
            )
            .extend(BT_A2DP_SINK_CHILD_SCHEMA)
            .extend(cv.COMPONENT_SCHEMA),
        },
        key=CONF_TYPE,
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BT_A2DP_SINK_ID])
    var = await switch.new_switch(config)
    await cg.register_parented(var, hub)

    if config[CONF_TYPE] == CONF_PAIRING_MODE:
        cg.add(hub.set_pairing_mode_switch(var))
    elif config[CONF_TYPE] == CONF_ALLOW_DEVICE_CONNECT:
        await cg.register_component(var, config)
        cg.add(hub.set_allow_connect_switch(var))
