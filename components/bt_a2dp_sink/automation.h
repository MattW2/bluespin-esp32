#pragma once

#include "esphome/core/automation.h"
#include "bt_a2dp_sink.h"

namespace esphome::bt_a2dp_sink {

template<typename... Ts> class StartPairingAction : public Action<Ts...> {
 public:
  explicit StartPairingAction(BtA2dpSink *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint32_t, duration_ms)

  void play(const Ts &...x) override {
    uint32_t dur = this->duration_ms_.has_value() ? this->duration_ms_.value(x...) : 0U;
    this->parent_->start_pairing(dur);
  }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class StopPairingAction : public Action<Ts...> {
 public:
  explicit StopPairingAction(BtA2dpSink *parent) : parent_(parent) {}
  void play(const Ts &.../*x*/) override { this->parent_->stop_pairing(); }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class DisconnectAction : public Action<Ts...> {
 public:
  explicit DisconnectAction(BtA2dpSink *parent) : parent_(parent) {}
  void play(const Ts &.../*x*/) override { this->parent_->disconnect_current(); }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class ReconnectLastAction : public Action<Ts...> {
 public:
  explicit ReconnectLastAction(BtA2dpSink *parent) : parent_(parent) {}
  void play(const Ts &.../*x*/) override { this->parent_->reconnect_last(); }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class ForgetAllAction : public Action<Ts...> {
 public:
  explicit ForgetAllAction(BtA2dpSink *parent) : parent_(parent) {}
  void play(const Ts &.../*x*/) override { this->parent_->forget_all_bonds(); }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class SetVolumeAction : public Action<Ts...> {
 public:
  explicit SetVolumeAction(BtA2dpSink *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(float, volume)

  void play(const Ts &...x) override {
    float v = this->volume_.value(x...);
    // cv.percentage produces 0.0..1.0; accept both 0..1 and 0..100 gracefully.
    if (v >= 0.0f && v <= 1.0f) {
      v *= 100.0f;
    }
    this->parent_->set_phone_volume_percent(v);
  }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class IsConnectedCondition : public Condition<Ts...> {
 public:
  explicit IsConnectedCondition(BtA2dpSink *parent) : parent_(parent) {}
  bool check(const Ts &.../*x*/) override { return this->parent_->is_connected(); }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class IsStreamingCondition : public Condition<Ts...> {
 public:
  explicit IsStreamingCondition(BtA2dpSink *parent) : parent_(parent) {}
  bool check(const Ts &.../*x*/) override { return this->parent_->is_streaming(); }

 private:
  BtA2dpSink *parent_;
};

template<typename... Ts> class IsPairingCondition : public Condition<Ts...> {
 public:
  explicit IsPairingCondition(BtA2dpSink *parent) : parent_(parent) {}
  bool check(const Ts &.../*x*/) override { return this->parent_->is_pairing_open(); }

 private:
  BtA2dpSink *parent_;
};

}  // namespace esphome::bt_a2dp_sink
