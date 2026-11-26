#pragma once

#include "esphome/components/button/button.h"
#include "esphome/core/helpers.h"
#include "../bt_a2dp_sink.h"

namespace esphome::bt_a2dp_sink {

enum class BtButtonType : uint8_t {
  START_PAIRING = 0,
  STOP_PAIRING,
  DISCONNECT,
  RECONNECT_LAST,
  FORGET_ALL,
};

class BtA2dpButton : public button::Button, public Parented<BtA2dpSink> {
 public:
  void set_button_type(BtButtonType type) { this->type_ = type; }

 protected:
  void press_action() override {
    switch (this->type_) {
      case BtButtonType::START_PAIRING:
        this->parent_->start_pairing();
        break;
      case BtButtonType::STOP_PAIRING:
        this->parent_->stop_pairing();
        break;
      case BtButtonType::DISCONNECT:
        this->parent_->disconnect_current();
        break;
      case BtButtonType::RECONNECT_LAST:
        this->parent_->reconnect_last();
        break;
      case BtButtonType::FORGET_ALL:
        this->parent_->forget_all_bonds();
        break;
    }
  }

  BtButtonType type_{BtButtonType::START_PAIRING};
};

}  // namespace esphome::bt_a2dp_sink
