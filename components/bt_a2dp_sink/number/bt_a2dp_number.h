#pragma once

#include "esphome/components/number/number.h"
#include "esphome/core/helpers.h"
#include "../bt_a2dp_sink.h"

namespace esphome::bt_a2dp_sink {

class BtPhoneVolumeNumber : public number::Number, public Parented<BtA2dpSink> {
 protected:
  void control(float value) override { this->parent_->set_phone_volume_percent(value); }
};

}  // namespace esphome::bt_a2dp_sink
