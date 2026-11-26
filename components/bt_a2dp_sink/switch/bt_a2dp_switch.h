#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "../bt_a2dp_sink.h"

namespace esphome::bt_a2dp_sink {

class BtPairingModeSwitch : public switch_::Switch, public Parented<BtA2dpSink> {
 protected:
  void write_state(bool state) override {
    if (state) {
      this->parent_->start_pairing();
    } else {
      this->parent_->stop_pairing();
    }
  }
};

class BtAllowConnectSwitch : public switch_::Switch, public Component, public Parented<BtA2dpSink> {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    bool initial = this->parent_->allow_device_connect();
    if (auto restored = this->get_initial_state_with_restore_mode()) {
      initial = *restored;
    }
    this->parent_->set_allow_device_connect(initial);
    this->publish_state(initial);
  }

 protected:
  void write_state(bool state) override { this->parent_->set_allow_device_connect(state); }
};

}  // namespace esphome::bt_a2dp_sink
