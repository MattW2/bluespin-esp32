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
static constexpr UBaseType_t kCaptureTaskPriority = 5;  // Match real-time audio priority on Core 1
static constexpr int64_t kFrameDurationUs = 20000;      // 882 frames @ 44100 Hz = exact 20,000 us

void BtA2dpMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 2, kOutputSampleRate);
  this->chunk_buf_.resize(kChunkBytes, 0);
  this->raw_buf_.resize((kFramesPerChunk + 1U) * kBytesPerStereoFrame, 0);
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
  auto *out_samples = reinterpret_cast<int16_t *>(chunk);
  auto *raw_samples = reinterpret_cast<int16_t *>(this->raw_buf_.data());
  auto &ring = this->parent_->ring();

  const size_t target_bytes = std::max<size_t>(this->parent_->target_buffer_bytes(), kChunkBytes * 4U);
  const size_t high_watermark = (target_bytes * 3U) / 2U;
  const size_t low_watermark = std::max<size_t>(target_bytes / 3U, kChunkBytes * 2U);
  const size_t hard_max_watermark = std::max<size_t>((target_bytes * 5U) / 2U, kChunkBytes * 10U);

  // Pre-roll up to 350 ms (half of target buffer) if Bluetooth is actively streaming so
  // 2.4 GHz Wi-Fi/BT coexistence jitter does not cause underruns.
  for (int i = 0; i < 35 && !this->stop_requested_.load(std::memory_order_acquire); ++i) {
    if (!this->parent_->is_streaming() || ring.available() >= target_bytes / 2U) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  int64_t chunk_start_us = esp_timer_get_time();
  int16_t last_l = 0;
  int16_t last_r = 0;

  while (!this->stop_requested_.load(std::memory_order_acquire)) {
    const size_t avail_before = ring.available();
    const bool bt_streaming = this->parent_->is_streaming();

    if (avail_before > hard_max_watermark) {
      // Emergency latency cap if producer got far ahead during a long stall.
      const size_t drop = (avail_before - target_bytes) & ~(kBytesPerStereoFrame - 1U);
      ring.skip(drop);
    }

    const size_t avail = ring.available();
    constexpr size_t kMidFrame = kFramesPerChunk / 2U;

    if (avail > high_watermark && avail >= (kFramesPerChunk + 1U) * kBytesPerStereoFrame) {
      // Source clock slightly faster than ESP32 clock: consume 883 frames and blend 2 middle frames into 882.
      ring.read(this->raw_buf_.data(), (kFramesPerChunk + 1U) * kBytesPerStereoFrame);
      std::memcpy(out_samples, raw_samples, kMidFrame * kBytesPerStereoFrame);
      out_samples[kMidFrame * 2U] = static_cast<int16_t>(
          (static_cast<int32_t>(raw_samples[kMidFrame * 2U]) + static_cast<int32_t>(raw_samples[(kMidFrame + 1U) * 2U])) /
          2);
      out_samples[kMidFrame * 2U + 1U] =
          static_cast<int16_t>((static_cast<int32_t>(raw_samples[kMidFrame * 2U + 1U]) +
                                static_cast<int32_t>(raw_samples[(kMidFrame + 1U) * 2U + 1U])) /
                               2);
      std::memcpy(&out_samples[(kMidFrame + 1U) * 2U], &raw_samples[(kMidFrame + 2U) * 2U],
                  (kFramesPerChunk - kMidFrame - 1U) * kBytesPerStereoFrame);
      last_l = out_samples[(kFramesPerChunk - 1U) * 2U];
      last_r = out_samples[(kFramesPerChunk - 1U) * 2U + 1U];
    } else if (bt_streaming && avail < low_watermark && avail >= (kFramesPerChunk - 1U) * kBytesPerStereoFrame) {
      // Source clock slightly slower than ESP32 clock (or rebuilding cushion): consume 881 frames and interpolate 1.
      ring.read(this->raw_buf_.data(), (kFramesPerChunk - 1U) * kBytesPerStereoFrame);
      std::memcpy(out_samples, raw_samples, kMidFrame * kBytesPerStereoFrame);
      out_samples[kMidFrame * 2U] = static_cast<int16_t>(
          (static_cast<int32_t>(raw_samples[(kMidFrame - 1U) * 2U]) + static_cast<int32_t>(raw_samples[kMidFrame * 2U])) /
          2);
      out_samples[kMidFrame * 2U + 1U] =
          static_cast<int16_t>((static_cast<int32_t>(raw_samples[(kMidFrame - 1U) * 2U + 1U]) +
                                static_cast<int32_t>(raw_samples[kMidFrame * 2U + 1U])) /
                               2);
      std::memcpy(&out_samples[(kMidFrame + 1U) * 2U], &raw_samples[kMidFrame * 2U],
                  (kFramesPerChunk - kMidFrame - 1U) * kBytesPerStereoFrame);
      last_l = out_samples[(kFramesPerChunk - 1U) * 2U];
      last_r = out_samples[(kFramesPerChunk - 1U) * 2U + 1U];
    } else {
      const size_t got = ring.read(chunk, kChunkBytes);
      const size_t got_frames = got / kBytesPerStereoFrame;
      if (got_frames > 0) {
        last_l = out_samples[(got_frames - 1U) * 2U];
        last_r = out_samples[(got_frames - 1U) * 2U + 1U];
      }
      if (got_frames < kFramesPerChunk) {
        if (bt_streaming && got_frames == 0) {
          ring.record_underrun();
        }
        // Smoothly decay last sample to zero to avoid hard step clicks on pause/underrun.
        for (size_t f = got_frames; f < kFramesPerChunk; ++f) {
          last_l = static_cast<int16_t>((static_cast<int32_t>(last_l) * 30) / 32);
          last_r = static_cast<int16_t>((static_cast<int32_t>(last_r) * 30) / 32);
          out_samples[f * 2U] = last_l;
          out_samples[f * 2U + 1U] = last_r;
        }
      }
    }

    // Strictly monotonic 20,000 us timestamp per 882-frame chunk (never jittered by A2DP packet burst size).
    const int64_t capture_time_us = chunk_start_us - kFrameDurationUs;
    this->publish_audio_(std::span<uint8_t>(chunk, kChunkBytes), this->audio_stream_info_, capture_time_us);

    chunk_start_us += kFrameDurationUs;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - chunk_start_us > 100000) {
      // Re-anchor cleanly if task scheduling ever stalled by >100 ms.
      chunk_start_us = now_us;
    }
    const int64_t sleep_us = chunk_start_us - now_us;
    if (sleep_us >= 1000) {
      vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>((sleep_us + 500) / 1000)));
    } else if (sleep_us > 0) {
      vTaskDelay(1);
    } else {
      taskYIELD();
    }
  }

  ring.clear();
  this->task_running_.store(false, std::memory_order_release);
}
#endif

}  // namespace esphome::bt_a2dp_sink
