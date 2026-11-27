#include "bt_a2dp_sink.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef USE_ESP32
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include <esp_coexist.h>
#endif
#endif

namespace esphome::bt_a2dp_sink {

static const char *const TAG = "bt_a2dp_sink";
static constexpr const char *kPairingTimeoutName = "bt_pairing_window";
static constexpr int64_t kAudioGapTimeoutUs = 600000;  // 600 ms without PCM frames -> stream idle
static constexpr size_t kMaxResampleFramesPerCallback = 2048;

BtA2dpSink *BtA2dpSink::instance_ = nullptr;

std::string BtA2dpSink::format_bda_(const uint8_t bda[6]) {
  char buf[18];
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
  return std::string(buf);
}

void BtA2dpSink::setup() {
  instance_ = this;

  if (this->device_name_.empty()) {
    this->device_name_ = App.get_friendly_name();
    if (this->device_name_.empty()) {
      this->device_name_ = App.get_name();
    }
  }

  // Allocate ring buffer sized to ~2.5x target buffer_duration_ms so transient jitter never overflows.
  this->target_buffer_bytes_ = (this->buffer_duration_ms_ * kOutputSampleRate * kBytesPerStereoFrame) / 1000U;
  const size_t min_ring_bytes = std::max<size_t>(this->target_buffer_bytes_ * 3U, 32768U);
  if (!this->ring_.allocate(min_ring_bytes)) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte PCM ring buffer", static_cast<unsigned>(min_ring_bytes));
    this->mark_failed();
    return;
  }
  this->resample_scratch_.resize(kMaxResampleFramesPerCallback * 2, 0);

  this->last_bda_pref_ = global_preferences->make_preference<std::array<uint8_t, 6>>(fnv1_hash("bt_a2dp_last_bda"));
  std::array<uint8_t, 6> saved_bda{};
  if (this->last_bda_pref_.load(&saved_bda)) {
    std::memcpy(this->last_bda_, saved_bda.data(), 6);
    this->has_last_bda_ = true;
  }

#ifdef USE_ESP32
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  if (nvs_err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(nvs_err));
    this->mark_failed();
    return;
  }

  // Release unused BLE controller memory (~30 KB DRAM) before initializing Classic BT.
  esp_err_t mem_err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  if (mem_err != ESP_OK && mem_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "esp_bt_controller_mem_release(BLE): %s", esp_err_to_name(mem_err));
  }

  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }

  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_enable(CLASSIC_BT) failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bd_cfg.ssp_en = true;
    esp_err_t err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bluedroid_init_with_cfg failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
    esp_err_t err = esp_bluedroid_enable();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }

  esp_bt_gap_register_callback(&BtA2dpSink::gap_cb_);
  esp_bt_gap_set_device_name(this->device_name_.c_str());

  // Pinless SSP "Just Works" (NoInputNoOutput) so headless sources like Google Nest Hub pair without a PIN prompt.
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

  // Variable PIN allows us to reject legacy PIN requests outside the pairing window and reply "0000" inside it.
  esp_bt_pin_code_t unused_pin{};
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unused_pin);

  // Initialize AVRCP before A2DP per ESP-IDF Bluedroid requirements.
  esp_avrc_ct_init();
  esp_avrc_ct_register_callback(&BtA2dpSink::avrc_ct_cb_);

  esp_avrc_tg_init();
  esp_avrc_tg_register_callback(&BtA2dpSink::avrc_tg_cb_);

  esp_avrc_rn_evt_cap_mask_t evt_set = {0};
  esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
  esp_avrc_tg_set_rn_evt_cap(&evt_set);

  esp_a2d_register_callback(&BtA2dpSink::a2d_cb_);
  esp_a2d_sink_register_data_callback(&BtA2dpSink::a2d_data_cb_);
  esp_err_t a2d_err = esp_a2d_sink_init();
  if (a2d_err != ESP_OK) {
    ESP_LOGE(TAG, "esp_a2d_sink_init failed: %s", esp_err_to_name(a2d_err));
    this->mark_failed();
    return;
  }

  // Prevent sendspin.source_task (2048-byte stack) from entering ESPHome's heavy vprintf logger hook on transient WARNs.
  esp_log_level_set("sendspin.source_task", ESP_LOG_ERROR);

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
#endif

  // Must be set after profile init so A2DP/AVRCP do not overwrite the Loudspeaker CoD needed by Google Nest Hub.
  this->apply_loudspeaker_cod_();
  this->stack_ready_ = true;
  this->update_scan_mode_();
  this->refresh_bonded_count_();
#endif

  // Publish initial entity states
#ifdef USE_BINARY_SENSOR
  if (this->connected_bs_ != nullptr)
    this->connected_bs_->publish_state(false);
  if (this->streaming_bs_ != nullptr)
    this->streaming_bs_->publish_state(false);
  if (this->pairing_mode_bs_ != nullptr)
    this->pairing_mode_bs_->publish_state(false);
#endif
#ifdef USE_SWITCH
  if (this->pairing_mode_sw_ != nullptr)
    this->pairing_mode_sw_->publish_state(false);
  if (this->allow_connect_sw_ != nullptr)
    this->allow_connect_sw_->publish_state(this->allow_device_connect_.load());
#endif
#ifdef USE_TEXT_SENSOR
  if (this->device_name_ts_ != nullptr)
    this->device_name_ts_->publish_state("None");
  if (this->device_address_ts_ != nullptr)
    this->device_address_ts_->publish_state("");
  if (this->codec_ts_ != nullptr)
    this->codec_ts_->publish_state("Idle");
#endif
#ifdef USE_NUMBER
  if (this->phone_volume_num_ != nullptr)
    this->phone_volume_num_->publish_state(this->get_phone_volume_percent());
#endif
  this->publish_diagnostics_();
}

void BtA2dpSink::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth A2DP Sink:");
  ESP_LOGCONFIG(TAG, "  Device Name: %s", this->device_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Pairing Window: %u s (Pinless SSP Just Works)",
                static_cast<unsigned>(this->pairing_window_ms_ / 1000U));
  ESP_LOGCONFIG(TAG, "  Max Bonded Devices: %u", static_cast<unsigned>(this->max_bonded_devices_));
  ESP_LOGCONFIG(TAG, "  Takeover (Last-One-Wins): %s", YESNO(this->takeover_enabled_));
  ESP_LOGCONFIG(TAG, "  PCM Ring Buffer: %u bytes (%s), target=%u ms",
                static_cast<unsigned>(this->ring_.capacity()),
                this->ring_.allocated_in_psram() ? "PSRAM" : "Internal RAM",
                static_cast<unsigned>(this->buffer_duration_ms_));
}

void BtA2dpSink::loop() {
  BtEvent ev;
  while (this->events_.pop(ev)) {
    this->process_event_(ev);
  }

#ifdef USE_ESP32
  // Detect silent stream stalls or sources that start/stop sending PCM without explicit AVDTP state changes.
  const int64_t now_us = esp_timer_get_time();
  const int64_t last_rx = this->last_audio_rx_us_.load(std::memory_order_acquire);
  const bool rx_recent = (last_rx > 0) && ((now_us - last_rx) < kAudioGapTimeoutUs);

  if (this->is_connected()) {
    if (!this->is_streaming() && this->a2dp_audio_started_.load(std::memory_order_acquire) && rx_recent) {
      this->set_streaming_state_(true);
    } else if (this->is_streaming() && !rx_recent && !this->a2dp_audio_started_.load(std::memory_order_acquire)) {
      this->set_streaming_state_(false);
    } else if (this->is_streaming() && last_rx > 0 && (now_us - last_rx) > (kAudioGapTimeoutUs * 3)) {
      // Source stopped delivering packets for >1.8s despite AVDTP staying open.
      this->set_streaming_state_(false);
    }
  }
#endif

  const uint32_t now_ms = millis();
  if (now_ms - this->last_diag_ms_ >= 5000U) {
    this->last_diag_ms_ = now_ms;
    this->publish_diagnostics_();
  }
}

void BtA2dpSink::start_pairing(uint32_t duration_ms) {
  if (!this->stack_ready_) {
    return;
  }
  const uint32_t window = duration_ms > 0 ? duration_ms : this->pairing_window_ms_;
  const bool was_open = this->pairing_open_.exchange(true, std::memory_order_acq_rel);

#ifdef USE_ESP32
  this->update_scan_mode_();
#endif

  this->set_timeout(kPairingTimeoutName, window, [this]() {
    ESP_LOGI(TAG, "Bluetooth pairing window expired");
    this->stop_pairing();
  });

  ESP_LOGI(TAG, "Opened pinless Bluetooth pairing window for %u s", static_cast<unsigned>(window / 1000U));
#ifdef USE_BINARY_SENSOR
  if (this->pairing_mode_bs_ != nullptr)
    this->pairing_mode_bs_->publish_state(true);
#endif
#ifdef USE_SWITCH
  if (this->pairing_mode_sw_ != nullptr)
    this->pairing_mode_sw_->publish_state(true);
#endif
  if (!was_open) {
    this->on_pairing_mode_changed_callbacks_.call(true);
  }
}

void BtA2dpSink::stop_pairing() {
  this->cancel_timeout(kPairingTimeoutName);
  const bool was_open = this->pairing_open_.exchange(false, std::memory_order_acq_rel);

#ifdef USE_ESP32
  if (this->stack_ready_) {
    this->update_scan_mode_();
  }
#endif

#ifdef USE_BINARY_SENSOR
  if (this->pairing_mode_bs_ != nullptr)
    this->pairing_mode_bs_->publish_state(false);
#endif
#ifdef USE_SWITCH
  if (this->pairing_mode_sw_ != nullptr)
    this->pairing_mode_sw_->publish_state(false);
#endif
  if (was_open) {
    ESP_LOGI(TAG, "Closed Bluetooth pairing window");
    this->on_pairing_mode_changed_callbacks_.call(false);
  }
}

void BtA2dpSink::disconnect_current() {
#ifdef USE_ESP32
  if (!this->stack_ready_ || !this->is_connected()) {
    return;
  }
  ESP_LOGI(TAG, "Disconnecting Bluetooth device %s", format_bda_(this->current_bda_).c_str());
  esp_a2d_sink_disconnect(this->current_bda_);
#endif
}

void BtA2dpSink::reconnect_last() {
#ifdef USE_ESP32
  if (!this->stack_ready_) {
    return;
  }
  if (!this->has_last_bda_) {
    // Fall back to most recently bonded device if available.
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
      ESP_LOGW(TAG, "reconnect_last: no bonded devices stored");
      return;
    }
    std::vector<esp_bd_addr_t> list(static_cast<size_t>(count));
    if (esp_bt_gap_get_bond_device_list(&count, list.data()) != ESP_OK || count <= 0) {
      return;
    }
    std::memcpy(this->last_bda_, list[0], 6);
    this->has_last_bda_ = true;
  }

  if (this->is_connected() && std::memcmp(this->current_bda_, this->last_bda_, 6) == 0) {
    ESP_LOGD(TAG, "reconnect_last: already connected to %s", format_bda_(this->last_bda_).c_str());
    return;
  }

  ESP_LOGI(TAG, "Initiating A2DP connection to %s", format_bda_(this->last_bda_).c_str());
  this->reconnect_in_progress_.store(true, std::memory_order_release);
  esp_a2d_sink_connect(this->last_bda_);
#endif
}

void BtA2dpSink::forget_all_bonds() {
#ifdef USE_ESP32
  if (!this->stack_ready_) {
    return;
  }
  if (this->is_connected()) {
    esp_a2d_sink_disconnect(this->current_bda_);
  }
  int count = esp_bt_gap_get_bond_device_num();
  if (count > 0) {
    std::vector<esp_bd_addr_t> list(static_cast<size_t>(count));
    if (esp_bt_gap_get_bond_device_list(&count, list.data()) == ESP_OK) {
      for (int i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "Removing bonded Bluetooth device %s", format_bda_(list[i]).c_str());
        esp_bt_gap_remove_bond_device(list[i]);
      }
    }
  }
  this->has_last_bda_ = false;
  std::memset(this->last_bda_, 0, sizeof(this->last_bda_));
  this->refresh_bonded_count_();
#endif
}

void BtA2dpSink::set_allow_device_connect(bool allow) {
  this->allow_device_connect_.store(allow, std::memory_order_release);
#ifdef USE_ESP32
  if (this->stack_ready_) {
    this->update_scan_mode_();
  }
#endif
#ifdef USE_SWITCH
  if (this->allow_connect_sw_ != nullptr) {
    this->allow_connect_sw_->publish_state(allow);
  }
#endif
  ESP_LOGI(TAG, "Allow bonded devices to initiate connection: %s", YESNO(allow));
}

void BtA2dpSink::set_phone_volume_percent(float percent) {
  const uint8_t avrcp_vol = percent_to_avrcp(percent);
  this->avrcp_volume_.store(avrcp_vol, std::memory_order_release);

#ifdef USE_ESP32
  if (this->stack_ready_ && this->is_connected() &&
      this->avrcp_rn_registered_.exchange(false, std::memory_order_acq_rel)) {
    esp_avrc_rn_param_t rn_param{};
    rn_param.volume = avrcp_vol;
    esp_err_t err = esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "esp_avrc_tg_send_rn_rsp(CHANGED, %u) failed: %s", avrcp_vol, esp_err_to_name(err));
    }
  }
#endif

  const float rounded_pct = static_cast<float>(avrcp_to_percent(avrcp_vol));
#ifdef USE_NUMBER
  if (this->phone_volume_num_ != nullptr) {
    this->phone_volume_num_->publish_state(rounded_pct);
  }
#endif
}

void BtA2dpSink::set_streaming_state_(bool streaming) {
  const bool prev = this->streaming_.exchange(streaming, std::memory_order_acq_rel);
  if (prev == streaming) {
    return;
  }
  ESP_LOGI(TAG, "Bluetooth audio stream %s", streaming ? "started" : "stopped");
#ifdef USE_ESP32
  if (this->stack_ready_) {
    this->update_scan_mode_();
  }
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
  if (streaming) {
    esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_PAUSED);
    esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING);
  } else {
    esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING);
    if (this->is_connected()) {
      esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_PAUSED);
    } else {
      esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_PAUSED);
    }
  }
#endif
#endif
#ifdef USE_BINARY_SENSOR
  if (this->streaming_bs_ != nullptr) {
    this->streaming_bs_->publish_state(streaming);
  }
#endif
  if (streaming) {
    this->on_stream_started_callbacks_.call();
  } else {
    this->on_stream_stopped_callbacks_.call();
  }
}

void BtA2dpSink::publish_diagnostics_() {
#ifdef USE_SENSOR
  if (this->buffer_fill_sensor_ != nullptr) {
    const float fill = this->ring_.fill_percent();
    if (this->last_published_buffer_fill_ < 0.0f || std::fabs(fill - this->last_published_buffer_fill_) >= 1.0f) {
      this->last_published_buffer_fill_ = fill;
      this->buffer_fill_sensor_->publish_state(fill);
    }
  }
  const uint32_t underruns = this->ring_.underrun_count();
  if (this->underruns_sensor_ != nullptr && underruns != this->last_published_underruns_) {
    this->last_published_underruns_ = underruns;
    this->underruns_sensor_->publish_state(static_cast<float>(underruns));
  }
  const uint32_t overflows = this->ring_.overflow_count();
  if (this->overflows_sensor_ != nullptr && overflows != this->last_published_overflows_) {
    this->last_published_overflows_ = overflows;
    this->overflows_sensor_->publish_state(static_cast<float>(overflows));
  }
#endif
}

void BtA2dpSink::process_event_(const BtEvent &ev) {
  switch (ev.type) {
    case BtEventType::A2DP_INIT_DONE: {
#ifdef USE_ESP32
      this->apply_loudspeaker_cod_();
      this->update_scan_mode_();
      this->refresh_bonded_count_();
#endif
      break;
    }

    case BtEventType::A2DP_CONNECTED: {
      std::memcpy(this->current_bda_, ev.bda, 6);
      std::memcpy(this->last_bda_, ev.bda, 6);
      this->has_last_bda_ = true;
      this->reconnect_in_progress_.store(false, std::memory_order_release);

      std::array<uint8_t, 6> to_save{};
      std::memcpy(to_save.data(), ev.bda, 6);
      this->last_bda_pref_.save(&to_save);

      this->connected_.store(true, std::memory_order_release);
      if (this->pairing_open_.load(std::memory_order_acquire)) {
        this->stop_pairing();
      }

      const std::string addr_str = format_bda_(ev.bda);
      if (this->current_remote_name_.empty()) {
        this->current_remote_name_ = addr_str;
      }
      ESP_LOGI(TAG, "A2DP connected: %s (%s)", this->current_remote_name_.c_str(), addr_str.c_str());

#ifdef USE_ESP32
      esp_bt_gap_read_remote_name(const_cast<uint8_t *>(ev.bda));
      this->update_scan_mode_();
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
      if (!this->is_streaming()) {
        esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_PAUSED);
      }
#endif
#endif

#ifdef USE_BINARY_SENSOR
      if (this->connected_bs_ != nullptr)
        this->connected_bs_->publish_state(true);
#endif
#ifdef USE_TEXT_SENSOR
      if (this->device_address_ts_ != nullptr)
        this->device_address_ts_->publish_state(addr_str);
      if (this->device_name_ts_ != nullptr)
        this->device_name_ts_->publish_state(this->current_remote_name_);
#endif
      this->on_connected_callbacks_.call();
      break;
    }

    case BtEventType::A2DP_DISCONNECTED: {
      // Ignore late disconnect notifications from an old peer after takeover already switched current_bda_.
      if (this->is_connected() && std::memcmp(this->current_bda_, ev.bda, 6) != 0) {
        ESP_LOGD(TAG, "Ignoring late disconnect for %s", format_bda_(ev.bda).c_str());
        break;
      }
      ESP_LOGI(TAG, "A2DP disconnected: %s", format_bda_(ev.bda).c_str());
      this->a2dp_audio_started_.store(false, std::memory_order_release);
      this->avrcp_rn_registered_.store(false, std::memory_order_release);
      this->last_audio_rx_us_.store(0, std::memory_order_release);
      this->set_streaming_state_(false);
      this->connected_.store(false, std::memory_order_release);
      std::memset(this->current_bda_, 0, sizeof(this->current_bda_));
      this->current_remote_name_.clear();

#ifdef USE_ESP32
      this->update_scan_mode_();
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
      esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING | ESP_COEX_BT_ST_A2DP_PAUSED);
#endif
#endif

#ifdef USE_BINARY_SENSOR
      if (this->connected_bs_ != nullptr)
        this->connected_bs_->publish_state(false);
#endif
#ifdef USE_TEXT_SENSOR
      if (this->device_name_ts_ != nullptr)
        this->device_name_ts_->publish_state("None");
      if (this->device_address_ts_ != nullptr)
        this->device_address_ts_->publish_state("");
      if (this->codec_ts_ != nullptr)
        this->codec_ts_->publish_state("Idle");
#endif
      this->on_disconnected_callbacks_.call();
      break;
    }

    case BtEventType::A2DP_AUDIO_STARTED: {
      this->a2dp_audio_started_.store(true, std::memory_order_release);
      this->set_streaming_state_(true);
      break;
    }

    case BtEventType::A2DP_AUDIO_SUSPENDED: {
      this->a2dp_audio_started_.store(false, std::memory_order_release);
      this->set_streaming_state_(false);
      break;
    }

    case BtEventType::A2DP_CODEC_CONFIGURED: {
      const uint32_t rate = ev.value;
      ESP_LOGI(TAG, "Negotiated A2DP SBC sample rate: %u Hz (Sendspin stream: %u Hz)", static_cast<unsigned>(rate),
               static_cast<unsigned>(kOutputSampleRate));
#ifdef USE_TEXT_SENSOR
      if (this->codec_ts_ != nullptr) {
        char desc[48];
        if (rate == kOutputSampleRate) {
          std::snprintf(desc, sizeof(desc), "SBC %.1f kHz", static_cast<double>(rate) / 1000.0);
        } else {
          std::snprintf(desc, sizeof(desc), "SBC %.1f kHz -> 44.1 kHz", static_cast<double>(rate) / 1000.0);
        }
        this->codec_ts_->publish_state(desc);
      }
#endif
      break;
    }

    case BtEventType::GAP_AUTH_SUCCESS: {
      if (ev.name[0] != '\0') {
        this->current_remote_name_ = ev.name;
#ifdef USE_TEXT_SENSOR
        if (this->is_connected() && std::memcmp(this->current_bda_, ev.bda, 6) == 0 &&
            this->device_name_ts_ != nullptr) {
          this->device_name_ts_->publish_state(this->current_remote_name_);
        }
#endif
      }
      ESP_LOGI(TAG, "Bluetooth authentication succeeded with %s (%s)", ev.name, format_bda_(ev.bda).c_str());
      if (this->pairing_open_.load(std::memory_order_acquire)) {
        this->stop_pairing();
      }
#ifdef USE_ESP32
      this->enforce_max_bonds_(ev.bda);
      this->refresh_bonded_count_();
#endif
      this->on_paired_callbacks_.call();
      break;
    }

    case BtEventType::GAP_AUTH_FAILED: {
      ESP_LOGW(TAG, "Bluetooth authentication failed with %s (status=0x%x)", format_bda_(ev.bda).c_str(),
               static_cast<unsigned>(ev.value));
      break;
    }

    case BtEventType::GAP_REMOTE_NAME: {
      if (ev.name[0] != '\0') {
        this->current_remote_name_ = ev.name;
        if (this->is_connected() && std::memcmp(this->current_bda_, ev.bda, 6) == 0) {
#ifdef USE_TEXT_SENSOR
          if (this->device_name_ts_ != nullptr) {
            this->device_name_ts_->publish_state(this->current_remote_name_);
          }
#endif
        }
      }
      break;
    }

    case BtEventType::GAP_BOND_REMOVED: {
#ifdef USE_ESP32
      this->refresh_bonded_count_();
#endif
      break;
    }

    case BtEventType::AVRCP_VOLUME_SET: {
      const uint8_t vol = static_cast<uint8_t>(ev.value & kAvrcpMaxVolume);
      const float pct = static_cast<float>(avrcp_to_percent(vol));
      ESP_LOGD(TAG, "Source changed AVRCP volume to %u/127 (%.0f%%)", vol, pct);
#ifdef USE_NUMBER
      if (this->phone_volume_num_ != nullptr) {
        this->phone_volume_num_->publish_state(pct);
      }
#endif
      this->on_volume_changed_callbacks_.call(pct);
      break;
    }

    case BtEventType::NONE:
      break;
  }
}

#ifdef USE_ESP32

void BtA2dpSink::apply_loudspeaker_cod_() {
  esp_bt_cod_t cod{};
  cod.major = ESP_BT_COD_MAJOR_DEV_AV;
  cod.minor = 0x05;  // Loudspeaker (Audio/Video minor class 0b000101) - recognized by Google Nest Hub / Google Home
  cod.service = ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO;
  esp_err_t err = esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_set_cod failed: %s", esp_err_to_name(err));
  }
}

void BtA2dpSink::update_scan_mode_() {
  const bool pairing = this->pairing_open_.load(std::memory_order_acquire);
  const bool allow_conn = this->allow_device_connect_.load(std::memory_order_acquire);
  const bool currently_conn = this->connected_.load(std::memory_order_acquire);
  const bool streaming = this->streaming_.load(std::memory_order_acquire);

  // Disable Page Scan while actively streaming audio so Classic BT Page Scan does not seize
  // the shared 2.4 GHz radio every 1.28 s and starve Sendspin Wi-Fi TX / A2DP RX.
  // When paused/idle (or when the pairing window is opened), connectability resumes for takeover.
  const bool connectable = pairing || (allow_conn && (!currently_conn || (this->takeover_enabled_ && !streaming)));
  const esp_bt_connection_mode_t c_mode = connectable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE;
  const esp_bt_discovery_mode_t d_mode = pairing ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE;

  esp_bt_gap_set_scan_mode(c_mode, d_mode);
}

bool BtA2dpSink::is_bonded_(const esp_bd_addr_t bda) const {
  int count = esp_bt_gap_get_bond_device_num();
  if (count <= 0) {
    return false;
  }
  std::vector<esp_bd_addr_t> list(static_cast<size_t>(count));
  if (esp_bt_gap_get_bond_device_list(&count, list.data()) != ESP_OK) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    if (std::memcmp(list[i], bda, 6) == 0) {
      return true;
    }
  }
  return false;
}

void BtA2dpSink::enforce_max_bonds_(const esp_bd_addr_t keep_bda) {
  int count = esp_bt_gap_get_bond_device_num();
  if (count <= static_cast<int>(this->max_bonded_devices_)) {
    return;
  }
  std::vector<esp_bd_addr_t> list(static_cast<size_t>(count));
  if (esp_bt_gap_get_bond_device_list(&count, list.data()) != ESP_OK) {
    return;
  }
  // Bluedroid returns most-recent first; evict from the tail while preserving `keep_bda` and `current_bda_`.
  for (int i = count - 1; i >= 0 && count > static_cast<int>(this->max_bonded_devices_); --i) {
    if (std::memcmp(list[i], keep_bda, 6) == 0) {
      continue;
    }
    if (this->is_connected() && std::memcmp(list[i], this->current_bda_, 6) == 0) {
      continue;
    }
    ESP_LOGI(TAG, "Evicting oldest bonded device %s (max_bonded_devices=%u)", format_bda_(list[i]).c_str(),
             this->max_bonded_devices_);
    esp_bt_gap_remove_bond_device(list[i]);
    --count;
  }
}

void BtA2dpSink::refresh_bonded_count_() {
#ifdef USE_SENSOR
  if (this->paired_devices_sensor_ != nullptr) {
    const int count = std::max(0, esp_bt_gap_get_bond_device_num());
    this->paired_devices_sensor_->publish_state(static_cast<float>(count));
  }
#endif
}

void BtA2dpSink::gap_cb_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (instance_ != nullptr && param != nullptr) {
    instance_->handle_gap_cb_(event, param);
  }
}

void BtA2dpSink::a2d_cb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (instance_ != nullptr && param != nullptr) {
    instance_->handle_a2d_cb_(event, param);
  }
}

void BtA2dpSink::a2d_data_cb_(const uint8_t *data, uint32_t len) {
  if (instance_ != nullptr) {
    instance_->handle_a2d_data_(data, len);
  }
}

void BtA2dpSink::avrc_ct_cb_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (instance_ != nullptr && param != nullptr) {
    instance_->handle_avrc_ct_cb_(event, param);
  }
}

void BtA2dpSink::avrc_tg_cb_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  if (instance_ != nullptr && param != nullptr) {
    instance_->handle_avrc_tg_cb_(event, param);
  }
}

void BtA2dpSink::handle_gap_cb_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT: {
      // Pinless SSP Just Works confirmation: accept ONLY while the pairing window is open.
      const bool accept = this->pairing_open_.load(std::memory_order_acquire);
      ESP_LOGI(TAG, "SSP pairing confirmation from %s -> %s", format_bda_(param->cfm_req.bda).c_str(),
               accept ? "ACCEPTED (pinless)" : "REJECTED (pairing window closed)");
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, accept);
      break;
    }

    case ESP_BT_GAP_PIN_REQ_EVT: {
      // Fallback for legacy pre-SSP Bluetooth 2.0 sources: reply "0000" only while pairing window is open.
      const bool accept = this->pairing_open_.load(std::memory_order_acquire) && !param->pin_req.min_16_digit;
      if (accept) {
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
      } else {
        esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, nullptr);
      }
      break;
    }

    case ESP_BT_GAP_KEY_REQ_EVT: {
      const bool accept = this->pairing_open_.load(std::memory_order_acquire);
      esp_bt_gap_ssp_passkey_reply(param->key_req.bda, accept, 0);
      break;
    }

    case ESP_BT_GAP_AUTH_CMPL_EVT: {
      BtEvent ev{};
      std::memcpy(ev.bda, param->auth_cmpl.bda, 6);
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        ev.type = BtEventType::GAP_AUTH_SUCCESS;
        std::strncpy(ev.name, reinterpret_cast<const char *>(param->auth_cmpl.device_name), sizeof(ev.name) - 1);
      } else {
        ev.type = BtEventType::GAP_AUTH_FAILED;
        ev.value = static_cast<uint32_t>(param->auth_cmpl.stat);
      }
      this->events_.push(ev);
      break;
    }

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
      if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
        BtEvent ev{};
        ev.type = BtEventType::GAP_REMOTE_NAME;
        std::memcpy(ev.bda, param->read_rmt_name.bda, 6);
        std::strncpy(ev.name, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name), sizeof(ev.name) - 1);
        this->events_.push(ev);
      }
      break;
    }

    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT: {
      BtEvent ev{};
      ev.type = BtEventType::GAP_BOND_REMOVED;
      std::memcpy(ev.bda, param->remove_bond_dev_cmpl.bda, 6);
      this->events_.push(ev);
      break;
    }

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
      if (param->acl_conn_cmpl_stat.stat != ESP_BT_STATUS_SUCCESS) {
        break;
      }
      const uint8_t *bda = param->acl_conn_cmpl_stat.bda;
      const bool pairing = this->pairing_open_.load(std::memory_order_acquire);
      const bool bonded = this->is_bonded_(bda);

      if (!pairing && !bonded) {
        ESP_LOGW(TAG, "Rejecting unbonded peer %s outside pairing window", format_bda_(bda).c_str());
        esp_a2d_sink_disconnect(const_cast<uint8_t *>(bda));
        break;
      }

      if (!pairing && !this->allow_device_connect_.load(std::memory_order_acquire) &&
          !this->reconnect_in_progress_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Rejecting peer-initiated connection from %s (allow_device_connect=off)",
                 format_bda_(bda).c_str());
        esp_a2d_sink_disconnect(const_cast<uint8_t *>(bda));
        break;
      }

      // Last-one-wins takeover: if another device is currently connected on A2DP, disconnect it so the new peer wins.
      if (this->takeover_enabled_ && this->connected_.load(std::memory_order_acquire) &&
          std::memcmp(this->current_bda_, bda, 6) != 0) {
        ESP_LOGI(TAG, "Takeover: disconnecting %s in favor of incoming %s", format_bda_(this->current_bda_).c_str(),
                 format_bda_(bda).c_str());
        esp_a2d_sink_disconnect(this->current_bda_);
      }
      break;
    }

    default:
      break;
  }
}

void BtA2dpSink::handle_a2d_cb_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  switch (event) {
    case ESP_A2D_PROF_STATE_EVT: {
      if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
        BtEvent ev{};
        ev.type = BtEventType::A2DP_INIT_DONE;
        this->events_.push(ev);
      }
      break;
    }

    case ESP_A2D_CONNECTION_STATE_EVT: {
      const auto state = param->conn_stat.state;
      const uint8_t *bda = param->conn_stat.remote_bda;

      if (state == ESP_A2D_CONNECTION_STATE_CONNECTING || state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        const bool pairing = this->pairing_open_.load(std::memory_order_acquire);
        if (!pairing && !this->is_bonded_(bda)) {
          esp_a2d_sink_disconnect(const_cast<uint8_t *>(bda));
          return;
        }
      }

      if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        BtEvent ev{};
        ev.type = BtEventType::A2DP_CONNECTED;
        std::memcpy(ev.bda, bda, 6);
        this->events_.push(ev);
      } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        BtEvent ev{};
        ev.type = BtEventType::A2DP_DISCONNECTED;
        std::memcpy(ev.bda, bda, 6);
        this->events_.push(ev);
      }
      break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
      BtEvent ev{};
      std::memcpy(ev.bda, param->audio_stat.remote_bda, 6);
      ev.type = (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) ? BtEventType::A2DP_AUDIO_STARTED
                                                                          : BtEventType::A2DP_AUDIO_SUSPENDED;
      this->events_.push(ev);
      break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
      if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
        const uint8_t sf = param->audio_cfg.mcc.cie.sbc_info.samp_freq;
        uint32_t rate = kOutputSampleRate;
        if (sf & ESP_A2D_SBC_CIE_SF_48K) {
          rate = 48000;
        } else if (sf & ESP_A2D_SBC_CIE_SF_44K) {
          rate = 44100;
        } else if (sf & ESP_A2D_SBC_CIE_SF_32K) {
          rate = 32000;
        } else if (sf & ESP_A2D_SBC_CIE_SF_16K) {
          rate = 16000;
        }
        this->negotiated_sample_rate_.store(rate, std::memory_order_release);
        this->resampler_.configure(rate, kOutputSampleRate);

        BtEvent ev{};
        ev.type = BtEventType::A2DP_CODEC_CONFIGURED;
        ev.value = rate;
        this->events_.push(ev);
      }
      break;
    }

    default:
      break;
  }
}

void BtA2dpSink::handle_a2d_data_(const uint8_t *data, uint32_t len) {
  if (data == nullptr || len < kBytesPerStereoFrame) {
    return;
  }
  this->last_audio_rx_us_.store(esp_timer_get_time(), std::memory_order_release);

  const uint32_t in_rate = this->negotiated_sample_rate_.load(std::memory_order_acquire);
  if (in_rate == kOutputSampleRate) {
    const size_t aligned_len = len & ~(kBytesPerStereoFrame - 1U);
    this->ring_.write(data, aligned_len);
    return;
  }

  // Resample chunk-by-chunk into 44.1 kHz stereo frames before writing to ring_.
  const auto *in_samples = reinterpret_cast<const int16_t *>(data);
  size_t remaining_frames = len / kBytesPerStereoFrame;
  const size_t max_out_frames = this->resample_scratch_.size() / 2;

  while (remaining_frames > 0) {
    const size_t chunk_in = std::min<size_t>(remaining_frames, 512U);
    const size_t out_frames =
        this->resampler_.process(in_samples, chunk_in, this->resample_scratch_.data(), max_out_frames);
    if (out_frames > 0) {
      this->ring_.write(reinterpret_cast<const uint8_t *>(this->resample_scratch_.data()),
                        out_frames * kBytesPerStereoFrame);
    }
    in_samples += chunk_in * 2;
    remaining_frames -= chunk_in;
  }
}

void BtA2dpSink::handle_avrc_ct_cb_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  (void) event;
  (void) param;
}

void BtA2dpSink::handle_avrc_tg_cb_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
      const uint8_t vol = param->set_abs_vol.volume & kAvrcpMaxVolume;
      this->avrcp_volume_.store(vol, std::memory_order_release);
      BtEvent ev{};
      ev.type = BtEventType::AVRCP_VOLUME_SET;
      ev.value = vol;
      this->events_.push(ev);
      break;
    }

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
      if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        this->avrcp_rn_registered_.store(true, std::memory_order_release);
        esp_avrc_rn_param_t rn_param{};
        rn_param.volume = this->avrcp_volume_.load(std::memory_order_acquire);
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
      }
      break;
    }

    case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
      if (!param->conn_stat.connected) {
        this->avrcp_rn_registered_.store(false, std::memory_order_release);
      }
      break;
    }

    default:
      break;
  }
}

#endif  // USE_ESP32

}  // namespace esphome::bt_a2dp_sink
