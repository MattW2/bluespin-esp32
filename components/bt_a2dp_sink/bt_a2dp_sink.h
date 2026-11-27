#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "audio_ring.h"
#include "resampler.h"
#include "volume_utils.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif

#ifdef USE_ESP32
#include <esp_a2dp_api.h>
#include <esp_a2dp_legacy_api.h>
#include <esp_avrc_api.h>
#include <esp_bt.h>
#include <esp_bt_defs.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#endif

namespace esphome::bt_a2dp_sink {

inline constexpr uint32_t kOutputSampleRate = 44100;
inline constexpr size_t kBytesPerStereoFrame = 4;  // 16-bit * 2 channels

enum class BtEventType : uint8_t {
  NONE = 0,
  A2DP_INIT_DONE,
  A2DP_CONNECTED,
  A2DP_DISCONNECTED,
  A2DP_AUDIO_STARTED,
  A2DP_AUDIO_SUSPENDED,
  A2DP_CODEC_CONFIGURED,
  GAP_AUTH_SUCCESS,
  GAP_AUTH_FAILED,
  GAP_REMOTE_NAME,
  GAP_BOND_REMOVED,
  AVRCP_VOLUME_SET,
};

struct BtEvent {
  BtEventType type{BtEventType::NONE};
  uint8_t bda[6]{};
  uint32_t value{0};
  char name[65]{};
};

/// Fixed-capacity lock-free SPSC event ring for BTC-task -> main-loop handoff (zero heap allocations).
template<size_t CAPACITY> class BtEventRing {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

 public:
  bool push(const BtEvent &ev) {
    const uint32_t tail = this->tail_.load(std::memory_order_relaxed);
    const uint32_t head = this->head_.load(std::memory_order_acquire);
    if (tail - head >= CAPACITY) {
      return false;
    }
    this->slots_[tail & (CAPACITY - 1)] = ev;
    this->tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool pop(BtEvent &out) {
    const uint32_t head = this->head_.load(std::memory_order_relaxed);
    const uint32_t tail = this->tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    out = this->slots_[head & (CAPACITY - 1)];
    this->head_.store(head + 1, std::memory_order_release);
    return true;
  }

 private:
  BtEvent slots_[CAPACITY]{};
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
};

class BtA2dpSink : public Component {
 public:
  BtA2dpSink() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // Configuration setters
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_pairing_window_ms(uint32_t ms) { this->pairing_window_ms_ = ms; }
  void set_max_bonded_devices(uint8_t count) { this->max_bonded_devices_ = count; }
  void set_allow_device_connect_default(bool allow) { this->allow_device_connect_ = allow; }
  void set_takeover_enabled(bool takeover) { this->takeover_enabled_ = takeover; }
  void set_buffer_duration_ms(uint32_t ms) { this->buffer_duration_ms_ = ms; }

  // Runtime control actions (main loop)
  void start_pairing(uint32_t duration_ms = 0);
  void stop_pairing();
  void disconnect_current();
  void reconnect_last();
  void forget_all_bonds();
  void set_allow_device_connect(bool allow);
  void set_phone_volume_percent(float percent);

  // State queries
  bool is_connected() const { return this->connected_.load(std::memory_order_acquire); }
  bool is_streaming() const { return this->streaming_.load(std::memory_order_acquire); }
  bool is_pairing_open() const { return this->pairing_open_.load(std::memory_order_acquire); }
  bool allow_device_connect() const { return this->allow_device_connect_.load(std::memory_order_acquire); }
  float get_phone_volume_percent() const {
    return static_cast<float>(avrcp_to_percent(this->avrcp_volume_.load(std::memory_order_acquire)));
  }

  AudioRingBuffer &ring() { return this->ring_; }
  const AudioRingBuffer &ring() const { return this->ring_; }
  uint32_t target_buffer_bytes() const { return this->target_buffer_bytes_; }

  /// Approximate microseconds of audio currently queued in the ring buffer (at 44.1 kHz stereo int16).
  int64_t buffered_microseconds() const {
    const size_t frames = this->ring_.available() / kBytesPerStereoFrame;
    return static_cast<int64_t>((static_cast<uint64_t>(frames) * 1000000ULL) / kOutputSampleRate);
  }

  // Entity registration hooks
#ifdef USE_BINARY_SENSOR
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->connected_bs_ = s; }
  void set_streaming_binary_sensor(binary_sensor::BinarySensor *s) { this->streaming_bs_ = s; }
  void set_pairing_mode_binary_sensor(binary_sensor::BinarySensor *s) { this->pairing_mode_bs_ = s; }
#endif
#ifdef USE_SENSOR
  void set_paired_devices_sensor(sensor::Sensor *s) { this->paired_devices_sensor_ = s; }
  void set_buffer_fill_sensor(sensor::Sensor *s) { this->buffer_fill_sensor_ = s; }
  void set_underruns_sensor(sensor::Sensor *s) { this->underruns_sensor_ = s; }
  void set_overflows_sensor(sensor::Sensor *s) { this->overflows_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_device_name_text_sensor(text_sensor::TextSensor *s) { this->device_name_ts_ = s; }
  void set_device_address_text_sensor(text_sensor::TextSensor *s) { this->device_address_ts_ = s; }
  void set_codec_text_sensor(text_sensor::TextSensor *s) { this->codec_ts_ = s; }
#endif
#ifdef USE_SWITCH
  void set_pairing_mode_switch(switch_::Switch *sw) { this->pairing_mode_sw_ = sw; }
  void set_allow_connect_switch(switch_::Switch *sw) { this->allow_connect_sw_ = sw; }
#endif
#ifdef USE_NUMBER
  void set_phone_volume_number(number::Number *num) { this->phone_volume_num_ = num; }
#endif

  // Automation callback hooks
  template<typename F> void add_on_connected_callback(F &&cb) {
    this->on_connected_callbacks_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_disconnected_callback(F &&cb) {
    this->on_disconnected_callbacks_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_stream_started_callback(F &&cb) {
    this->on_stream_started_callbacks_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_stream_stopped_callback(F &&cb) {
    this->on_stream_stopped_callbacks_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_pairing_mode_changed_callback(F &&cb) {
    this->on_pairing_mode_changed_callbacks_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_paired_callback(F &&cb) { this->on_paired_callbacks_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_volume_changed_callback(F &&cb) {
    this->on_volume_changed_callbacks_.add(std::forward<F>(cb));
  }

 protected:
#ifdef USE_ESP32
  static void gap_cb_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void a2d_cb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  static void a2d_data_cb_(const uint8_t *data, uint32_t len);
  static void avrc_ct_cb_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
  static void avrc_tg_cb_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

  void handle_gap_cb_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  void handle_a2d_cb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  void handle_a2d_data_(const uint8_t *data, uint32_t len);
  void handle_avrc_ct_cb_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
  void handle_avrc_tg_cb_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

  void apply_loudspeaker_cod_();
  void update_scan_mode_();
  bool is_bonded_(const esp_bd_addr_t bda) const;
  void enforce_max_bonds_(const esp_bd_addr_t keep_bda);
  void refresh_bonded_count_();
#endif

  void process_event_(const BtEvent &ev);
  void set_streaming_state_(bool streaming);
  void publish_diagnostics_();
  static std::string format_bda_(const uint8_t bda[6]);

  static BtA2dpSink *instance_;

  std::string device_name_;
  uint32_t pairing_window_ms_{120000};
  uint8_t max_bonded_devices_{8};
  uint32_t buffer_duration_ms_{300};
  uint32_t target_buffer_bytes_{0};
  bool takeover_enabled_{true};
  bool stack_ready_{false};

  std::atomic<bool> pairing_open_{false};
  std::atomic<bool> allow_device_connect_{true};
  std::atomic<bool> reconnect_in_progress_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> streaming_{false};
  std::atomic<bool> a2dp_audio_started_{false};
  std::atomic<uint32_t> negotiated_sample_rate_{kOutputSampleRate};
  std::atomic<int64_t> last_audio_rx_us_{0};
  std::atomic<uint8_t> avrcp_volume_{96};  // ~75% default until phone or HA sets it
  std::atomic<bool> avrcp_rn_registered_{false};

  uint8_t current_bda_[6]{};
  uint8_t last_bda_[6]{};
  bool has_last_bda_{false};
  std::string current_remote_name_;
  ESPPreferenceObject last_bda_pref_;

  AudioRingBuffer ring_;
  StereoLinearResampler resampler_;
  std::vector<int16_t> resample_scratch_;
  BtEventRing<32> events_;

  uint32_t last_diag_ms_{0};
  float last_published_buffer_fill_{-1.0f};
  uint32_t last_published_underruns_{UINT32_MAX};
  uint32_t last_published_overflows_{UINT32_MAX};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_bs_{nullptr};
  binary_sensor::BinarySensor *streaming_bs_{nullptr};
  binary_sensor::BinarySensor *pairing_mode_bs_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *paired_devices_sensor_{nullptr};
  sensor::Sensor *buffer_fill_sensor_{nullptr};
  sensor::Sensor *underruns_sensor_{nullptr};
  sensor::Sensor *overflows_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *device_name_ts_{nullptr};
  text_sensor::TextSensor *device_address_ts_{nullptr};
  text_sensor::TextSensor *codec_ts_{nullptr};
#endif
#ifdef USE_SWITCH
  switch_::Switch *pairing_mode_sw_{nullptr};
  switch_::Switch *allow_connect_sw_{nullptr};
#endif
#ifdef USE_NUMBER
  number::Number *phone_volume_num_{nullptr};
#endif

  CallbackManager<void()> on_connected_callbacks_;
  CallbackManager<void()> on_disconnected_callbacks_;
  CallbackManager<void()> on_stream_started_callbacks_;
  CallbackManager<void()> on_stream_stopped_callbacks_;
  CallbackManager<void(bool)> on_pairing_mode_changed_callbacks_;
  CallbackManager<void()> on_paired_callbacks_;
  CallbackManager<void(float)> on_volume_changed_callbacks_;
};

}  // namespace esphome::bt_a2dp_sink

#include "automation.h"
