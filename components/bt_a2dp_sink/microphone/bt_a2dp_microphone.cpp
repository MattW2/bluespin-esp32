#include "bt_a2dp_microphone.h"

#include "esphome/core/log.h"

#include <cstring>
#include <span>

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif

namespace esphome::bt_a2dp_sink {

static const char *const TAG = "bt_a2dp.microphone";
static constexpr size_t kCaptureTaskStackBytes = 6144;
static constexpr UBaseType_t kCaptureTaskPriority = 4;  // Above Sendspin source task (3), below protocol task (5)
static constexpr size_t kDriftDropBytes = 44 * kBytesPerStereoFrame;  // 1 ms (44 frames) shed on clock drift

void BtA2dpMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 2, kOutputSampleRate);
  this->chunk_buf_.resize(kChunkBytes, 0);
  this->state_ = microphone::STATE_STOPPED;
}

void BtA2dpMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth A2DP Virtual Microphone:");
  ESP_LOGCONFIG(TAG, "  Format: %u Hz, %u-bit, %u channels (%u bytes / %u ms frame)",
                static_cast<unsigned>(kOutputSampleRate), 16U, 2U, static_cast<unsigned>(kChunkBytes),
                static_cast<unsigned>(kFrameDurationMs));
  ESP_LOGCONFIG(TAG, "  Task Stack in PSRAM: %s", YESNO(this->task_stack_in_psram_));
}

void BtA2dpMicrophone::loop() {
  if (this->state_ == microphone::STATE_STARTING && this->task_running_.load(std::memory_order_acquire)) {
    this->state_ = microphone::STATE_RUNNING;
  } else if (this->state_ == microphone::STATE_STOPPING && !this->task_running_.load(std::memory_order_acquire)) {
    this->task_handle_ = nullptr;
    this->state_ = microphone::STATE_STOPPED;
  }
}

void BtA2dpMicrophone::start() {
  if (this->state_ == microphone::STATE_RUNNING || this->state_ == microphone::STATE_STARTING) {
    return;
  }
#ifdef USE_ESP32
  // Wait briefly if a previous stop is still winding down.
  for (int i = 0; i < 10 && this->task_running_.load(std::memory_order_acquire); ++i) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (this->task_running_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Previous capture task still exiting; deferring start");
    return;
  }

  this->parent_->ring().clear();
  this->stop_requested_.store(false, std::memory_order_release);
  this->state_ = microphone::STATE_STARTING;

  BaseType_t created = pdFAIL;
  if (this->task_stack_in_psram_) {
    created = xTaskCreatePinnedToCoreWithCaps(&BtA2dpMicrophone::capture_task_entry_, "bt_a2dp_mic",
                                              kCaptureTaskStackBytes, this, kCaptureTaskPriority, &this->task_handle_,
                                              1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (created != pdPASS) {
    created = xTaskCreatePinnedToCore(&BtA2dpMicrophone::capture_task_entry_, "bt_a2dp_mic", kCaptureTaskStackBytes,
                                      this, kCaptureTaskPriority, &this->task_handle_, 1);
  }
  if (created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create bt_a2dp_mic FreeRTOS task");
    this->state_ = microphone::STATE_STOPPED;
    return;
  }
  ESP_LOGI(TAG, "Started Sendspin Bluetooth capture stream");
#endif
}

void BtA2dpMicrophone::stop() {
  if (this->state_ == microphone::STATE_STOPPED || this->state_ == microphone::STATE_STOPPING) {
    return;
  }
  this->stop_requested_.store(true, std::memory_order_release);
  this->state_ = microphone::STATE_STOPPING;
  ESP_LOGI(TAG, "Stopping Sendspin Bluetooth capture stream");
}

#ifdef USE_ESP32
void BtA2dpMicrophone::capture_task_entry_(void *arg) {
  auto *self = static_cast<BtA2dpMicrophone *>(arg);
  self->capture_task_loop_();
  vTaskDelete(nullptr);
}

void BtA2dpMicrophone::capture_task_loop_() {
  this->task_running_.store(true, std::memory_order_release);

  uint8_t *chunk = this->chunk_buf_.data();
  auto &ring = this->parent_->ring();
  const size_t high_watermark = std::max<size_t>(this->parent_->target_buffer_bytes() * 2U, kChunkBytes * 4U);

  // Pre-roll up to 250 ms (half of target buffer) if Bluetooth is actively streaming so
  // 2.4 GHz Wi-Fi/BT coexistence jitter does not cause immediate underruns.
  for (int i = 0; i < 25 && !this->stop_requested_.load(std::memory_order_acquire); ++i) {
    if (!this->parent_->is_streaming() || ring.available() >= this->parent_->target_buffer_bytes() / 2U) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period_ticks = pdMS_TO_TICKS(kFrameDurationMs);

  while (!this->stop_requested_.load(std::memory_order_acquire)) {
    const size_t avail_before = ring.available();
    if (avail_before > high_watermark) {
      // Source clock slightly faster than local ESP32 clock: drop 1 ms so buffer latency stays bounded.
      ring.skip(kDriftDropBytes);
    }

    const int64_t buffered_us = this->parent_->buffered_microseconds();
    const int64_t now_us = esp_timer_get_time();
    const size_t got = ring.read(chunk, kChunkBytes);

    if (got < kChunkBytes) {
      std::memset(chunk + got, 0, kChunkBytes - got);
      if (this->parent_->is_streaming() && got == 0) {
        ring.record_underrun();
      }
    }

    const int64_t capture_time_us = (got > 0) ? (now_us - buffered_us) : (now_us - (kFrameDurationMs * 1000LL));
    this->publish_audio_(std::span<uint8_t>(chunk, kChunkBytes), this->audio_stream_info_, capture_time_us);

    vTaskDelayUntil(&last_wake, period_ticks);
  }

  ring.clear();
  this->task_running_.store(false, std::memory_order_release);
}
#endif

}  // namespace esphome::bt_a2dp_sink
