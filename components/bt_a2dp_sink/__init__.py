from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_DURATION,
    CONF_ID,
    CONF_VOLUME,
)
from esphome.core import CORE, TimePeriod
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["esp32"]
CONFLICTS_WITH = [
    "esp32_ble",
    "esp32_ble_tracker",
    "esp32_ble_beacon",
    "esp32_ble_server",
    "bluetooth_proxy",
]

DOMAIN = "bt_a2dp_sink"

CONF_BT_A2DP_SINK_ID = "bt_a2dp_sink_id"
CONF_DEVICE_NAME = "device_name"
CONF_PAIRING_WINDOW = "pairing_window"
CONF_MAX_BONDED_DEVICES = "max_bonded_devices"
CONF_ALLOW_DEVICE_CONNECT = "allow_device_connect"
CONF_TAKEOVER = "takeover"
CONF_BUFFER_DURATION = "buffer_duration"

CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_STREAM_STARTED = "on_stream_started"
CONF_ON_STREAM_STOPPED = "on_stream_stopped"
CONF_ON_PAIRING_MODE_CHANGED = "on_pairing_mode_changed"
CONF_ON_PAIRED = "on_paired"
CONF_ON_VOLUME_CHANGED = "on_volume_changed"

bt_a2dp_sink_ns = cg.esphome_ns.namespace("bt_a2dp_sink")
BtA2dpSink = bt_a2dp_sink_ns.class_("BtA2dpSink", cg.Component)

StartPairingAction = bt_a2dp_sink_ns.class_("StartPairingAction", automation.Action)
StopPairingAction = bt_a2dp_sink_ns.class_("StopPairingAction", automation.Action)
DisconnectAction = bt_a2dp_sink_ns.class_("DisconnectAction", automation.Action)
ReconnectLastAction = bt_a2dp_sink_ns.class_("ReconnectLastAction", automation.Action)
ForgetAllAction = bt_a2dp_sink_ns.class_("ForgetAllAction", automation.Action)
SetVolumeAction = bt_a2dp_sink_ns.class_("SetVolumeAction", automation.Action)

IsConnectedCondition = bt_a2dp_sink_ns.class_(
    "IsConnectedCondition", automation.Condition
)
IsStreamingCondition = bt_a2dp_sink_ns.class_(
    "IsStreamingCondition", automation.Condition
)
IsPairingCondition = bt_a2dp_sink_ns.class_("IsPairingCondition", automation.Condition)

BT_A2DP_SINK_CHILD_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BT_A2DP_SINK_ID): cv.use_id(BtA2dpSink),
    }
)


def _validate_classic_bt_variant(config: ConfigType) -> ConfigType:
    variant = esp32.get_esp32_variant()
    if variant != esp32.VARIANT_ESP32:
        raise cv.Invalid(
            f"bt_a2dp_sink requires an original ESP32 with Bluetooth Classic (BR/EDR), "
            f"but configured variant is '{variant}' (BLE-only). Use an ESP32 / ESP32-WROVER / ESP32-POE2 board."
        )
    return config


def _final_validate(config: ConfigType) -> ConfigType:
    full_conf = fv.full_config.get()
    for conflicting in CONFLICTS_WITH:
        if conflicting in full_conf:
            raise cv.Invalid(
                f"bt_a2dp_sink runs the ESP32 radio in Classic Bluetooth (BR/EDR-only) mode "
                f"and cannot be combined with '{conflicting}'."
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_CONNECTED, "add_on_connected_callback"),
    automation.CallbackAutomation(CONF_ON_DISCONNECTED, "add_on_disconnected_callback"),
    automation.CallbackAutomation(
        CONF_ON_STREAM_STARTED, "add_on_stream_started_callback"
    ),
    automation.CallbackAutomation(
        CONF_ON_STREAM_STOPPED, "add_on_stream_stopped_callback"
    ),
    automation.CallbackAutomation(
        CONF_ON_PAIRING_MODE_CHANGED,
        "add_on_pairing_mode_changed_callback",
        [(bool, "open")],
    ),
    automation.CallbackAutomation(CONF_ON_PAIRED, "add_on_paired_callback"),
    automation.CallbackAutomation(
        CONF_ON_VOLUME_CHANGED,
        "add_on_volume_changed_callback",
        [(float, "volume")],
    ),
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BtA2dpSink),
            cv.Optional(CONF_DEVICE_NAME): cv.All(
                cv.string_strict, cv.Length(min=1), cv.ByteLength(max=64)
            ),
            cv.Optional(
                CONF_PAIRING_WINDOW, default="120s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_BONDED_DEVICES, default=8): cv.int_range(
                min=1, max=20
            ),
            cv.Optional(CONF_ALLOW_DEVICE_CONNECT, default=True): cv.boolean,
            cv.Optional(CONF_TAKEOVER, default=True): cv.boolean,
            cv.Optional(CONF_BUFFER_DURATION, default="300ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=TimePeriod(milliseconds=60),
                    max=TimePeriod(milliseconds=2000),
                ),
            ),
            cv.Optional(CONF_ON_CONNECTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_STREAM_STARTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_STREAM_STOPPED): automation.validate_automation({}),
            cv.Optional(CONF_ON_PAIRING_MODE_CHANGED): automation.validate_automation(
                {}
            ),
            cv.Optional(CONF_ON_PAIRED): automation.validate_automation({}),
            cv.Optional(CONF_ON_VOLUME_CHANGED): automation.validate_automation({}),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
    _validate_classic_bt_variant,
)


async def to_code(config: ConfigType) -> None:
    esp32.request_bluetooth()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_DEVICE_NAME in config:
        cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_pairing_window_ms(config[CONF_PAIRING_WINDOW]))
    cg.add(var.set_max_bonded_devices(config[CONF_MAX_BONDED_DEVICES]))
    cg.add(var.set_allow_device_connect_default(config[CONF_ALLOW_DEVICE_CONNECT]))
    cg.add(var.set_takeover_enabled(config[CONF_TAKEOVER]))
    cg.add(var.set_buffer_duration_ms(config[CONF_BUFFER_DURATION]))

    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)

    sdkconfig_options = {
        "CONFIG_BT_ENABLED": True,
        "CONFIG_BT_BLUEDROID_ENABLED": True,
        "CONFIG_BT_CLASSIC_ENABLED": True,
        "CONFIG_BT_A2DP_ENABLE": True,
        "CONFIG_BT_A2DP_USE_EXTERNAL_CODEC": False,
        "CONFIG_BT_AVRCP_ENABLED": True,
        "CONFIG_BT_SSP_ENABLED": True,
        "CONFIG_BT_BLE_ENABLED": False,
        "CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY": True,
        "CONFIG_BTDM_CTRL_MODE_BTDM": False,
        "CONFIG_BTDM_CTRL_MODE_BLE_ONLY": False,
        "CONFIG_BT_ACL_CONNECTIONS": 2,
        "CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST": True,
        "CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY": True,
    }
    for key, val in sdkconfig_options.items():
        esp32.add_idf_sdkconfig_option(key, val)

    if "wifi" in CORE.config:
        esp32.request_software_coexistence()

    cg.add_define("USE_BT_A2DP_SINK")


_ACTION_BASE_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(BtA2dpSink),
    }
)


@automation.register_action(
    "bt_a2dp_sink.start_pairing",
    StartPairingAction,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(BtA2dpSink),
            cv.Optional(CONF_DURATION): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def start_pairing_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    if CONF_DURATION in config:
        templ = await cg.templatable(config[CONF_DURATION], args, cg.uint32)
        cg.add(var.set_duration_ms(templ))
    return var


@automation.register_action(
    "bt_a2dp_sink.stop_pairing",
    StopPairingAction,
    _ACTION_BASE_SCHEMA,
    synchronous=True,
)
async def stop_pairing_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "bt_a2dp_sink.disconnect",
    DisconnectAction,
    _ACTION_BASE_SCHEMA,
    synchronous=True,
)
async def disconnect_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "bt_a2dp_sink.reconnect_last",
    ReconnectLastAction,
    _ACTION_BASE_SCHEMA,
    synchronous=True,
)
async def reconnect_last_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "bt_a2dp_sink.forget_all",
    ForgetAllAction,
    _ACTION_BASE_SCHEMA,
    synchronous=True,
)
async def forget_all_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "bt_a2dp_sink.set_volume",
    SetVolumeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(BtA2dpSink),
            cv.Required(CONF_VOLUME): cv.templatable(cv.percentage),
        }
    ),
    synchronous=True,
)
async def set_volume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    templ = await cg.templatable(config[CONF_VOLUME], args, cg.float_)
    cg.add(var.set_volume(templ))
    return var


@automation.register_condition(
    "bt_a2dp_sink.is_connected", IsConnectedCondition, _ACTION_BASE_SCHEMA
)
async def is_connected_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)


@automation.register_condition(
    "bt_a2dp_sink.is_streaming", IsStreamingCondition, _ACTION_BASE_SCHEMA
)
async def is_streaming_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)


@automation.register_condition(
    "bt_a2dp_sink.is_pairing", IsPairingCondition, _ACTION_BASE_SCHEMA
)
async def is_pairing_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)
