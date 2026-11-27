#pragma once

#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../bt_a2dp_sink.h"

#include <atomic>
#include <cstdint>
#include <vector>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome::bt_a2dp_sink {

/**
 * Virtual ESPHome Microphone fed by BtA2dpSink's PCM ring buffer.
 *
 * Publishes continuous 20 ms stereo 16-bit 44.1 kHz frames (882 frames = 3528 bytes)
 * when started by Sendspin's source@v1 role. Fills silence while the Bluetooth source
 * is paused so Music Assistant's Live Input stays open for seamless resume.
 */
class BtA2dpMicrophone : public microphone::Microphone, public Component, public Parented<BtA2dpSink> {
 public:
  static constexpr uint32_t kFrameDurationMs = 20;
  static constexpr size_t kFramesPerChunk = (kOutputSampleRate * kFrameDurationMs) / 1000;  // 882 frames (exact)
  static constexpr size_t kChunkBytes = kFramesPerChunk * kBytesPerStereoFrame;             // 3528 bytes

  void setup() override;
  void loop() override;
  void dump_config() override;

  void start() override;
  void stop() override;

  void set_task_stack_in_psram(bool in_psram) { this->task_stack_in_psram_ = in_psram; }

 protected:
#ifdef USE_ESP32
  static void capture_task_entry_(void *arg);
  void capture_task_loop_();

  TaskHandle_t task_handle_{nullptr};
#endif

  bool task_stack_in_psram_{true};
  std::vector<uint8_t> chunk_buf_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> task_running_{false};
};

}  // namespace esphome::bt_a2dp_sink
