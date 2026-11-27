import esphome.codegen as cg
from esphome.components import audio, microphone, psram
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
    CONF_TASK_STACK_IN_PSRAM,
)
from esphome.types import ConfigType

from .. import BT_A2DP_SINK_CHILD_SCHEMA, CONF_BT_A2DP_SINK_ID, bt_a2dp_sink_ns

CODEOWNERS = ["@bluespin-esp32"]
DEPENDENCIES = ["bt_a2dp_sink"]

BtA2dpMicrophone = bt_a2dp_sink_ns.class_(
    "BtA2dpMicrophone",
    microphone.Microphone,
    cg.Component,
)

FIXED_SAMPLE_RATE = 48000
FIXED_BITS_PER_SAMPLE = 16
FIXED_CHANNELS = 2


def _set_fixed_audio_params(config: ConfigType) -> ConfigType:
    config[CONF_SAMPLE_RATE] = FIXED_SAMPLE_RATE
    config[CONF_BITS_PER_SAMPLE] = FIXED_BITS_PER_SAMPLE
    config[CONF_NUM_CHANNELS] = FIXED_CHANNELS
    audio.set_stream_limits(
        min_bits_per_sample=FIXED_BITS_PER_SAMPLE,
        max_bits_per_sample=FIXED_BITS_PER_SAMPLE,
        min_channels=FIXED_CHANNELS,
        max_channels=FIXED_CHANNELS,
        min_sample_rate=FIXED_SAMPLE_RATE,
        max_sample_rate=FIXED_SAMPLE_RATE,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(BtA2dpMicrophone),
            cv.Optional(CONF_TASK_STACK_IN_PSRAM, default=True): cv.boolean,
        }
    )
    .extend(BT_A2DP_SINK_CHILD_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _set_fixed_audio_params,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_BT_A2DP_SINK_ID])
    await microphone.register_microphone(var, config)
    in_psram = config[CONF_TASK_STACK_IN_PSRAM]
    cg.add(var.set_task_stack_in_psram(in_psram))
    if in_psram:
        psram.request_external_task_stack()
