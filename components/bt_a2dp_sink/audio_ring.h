#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(USE_ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace esphome::bt_a2dp_sink {

/**
 * Lock-free Single-Producer Single-Consumer (SPSC) byte ring buffer for PCM audio.
 *
 * Producer: Bluedroid BTC callback task (calls write()).
 * Consumer: Virtual microphone capture task (calls read(), skip(), clear()).
 *
 * Uses monotonically increasing power-of-two masked indices so the full capacity is usable
 * and consumer-side clear() remains lock-free even while the producer is active.
 */
class AudioRingBuffer {
 public:
  AudioRingBuffer() = default;
  ~AudioRingBuffer() { this->deallocate(); }

  AudioRingBuffer(const AudioRingBuffer &) = delete;
  AudioRingBuffer &operator=(const AudioRingBuffer &) = delete;

  bool allocate(size_t min_bytes) {
    this->deallocate();
    if (min_bytes < 64) {
      min_bytes = 64;
    }
    size_t cap = 64;
    while (cap < min_bytes) {
      if (cap > (SIZE_MAX >> 1)) {
        return false;
      }
      cap <<= 1;
    }

#if defined(USE_ESP32) || defined(ESP_PLATFORM)
    this->buffer_ = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (this->buffer_ != nullptr) {
      this->in_psram_ = true;
    } else {
      this->buffer_ = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      this->in_psram_ = false;
    }
#else
    this->buffer_ = static_cast<uint8_t *>(std::malloc(cap));
    this->in_psram_ = false;
#endif

    if (this->buffer_ == nullptr) {
      return false;
    }

    std::memset(this->buffer_, 0, cap);
    this->capacity_ = cap;
    this->mask_ = cap - 1;
    this->head_.store(0, std::memory_order_relaxed);
    this->tail_.store(0, std::memory_order_relaxed);
    this->overflow_count_.store(0, std::memory_order_relaxed);
    this->underrun_count_.store(0, std::memory_order_relaxed);
    return true;
  }

  void deallocate() {
    if (this->buffer_ != nullptr) {
#if defined(USE_ESP32) || defined(ESP_PLATFORM)
      heap_caps_free(this->buffer_);
#else
      std::free(this->buffer_);
#endif
      this->buffer_ = nullptr;
    }
    this->capacity_ = 0;
    this->mask_ = 0;
    this->in_psram_ = false;
  }

  /// Producer-only: non-blocking all-or-nothing write. Returns false and increments overflow_count on insufficient space.
  bool write(const uint8_t *data, size_t len) {
    if (this->buffer_ == nullptr || data == nullptr || len == 0) {
      return false;
    }
    if (len > this->capacity_) {
      this->overflow_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    const size_t tail = this->tail_.load(std::memory_order_relaxed);
    const size_t head = this->head_.load(std::memory_order_acquire);
    const size_t used = tail - head;
    const size_t free_space = this->capacity_ - used;

    if (len > free_space) {
      this->overflow_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    const size_t offset = tail & this->mask_;
    const size_t first_chunk = std::min(len, this->capacity_ - offset);
    std::memcpy(this->buffer_ + offset, data, first_chunk);
    if (first_chunk < len) {
      std::memcpy(this->buffer_, data + first_chunk, len - first_chunk);
    }

    this->tail_.store(tail + len, std::memory_order_release);
    return true;
  }

  /// Consumer-only: reads up to `len` bytes into `dst` and returns actual bytes read.
  size_t read(uint8_t *dst, size_t len) {
    if (this->buffer_ == nullptr || dst == nullptr || len == 0) {
      return 0;
    }

    const size_t head = this->head_.load(std::memory_order_relaxed);
    const size_t tail = this->tail_.load(std::memory_order_acquire);
    const size_t avail = tail - head;
    const size_t to_read = std::min(len, avail);
    if (to_read == 0) {
      return 0;
    }

    const size_t offset = head & this->mask_;
    const size_t first_chunk = std::min(to_read, this->capacity_ - offset);
    std::memcpy(dst, this->buffer_ + offset, first_chunk);
    if (first_chunk < to_read) {
      std::memcpy(dst + first_chunk, this->buffer_, to_read - first_chunk);
    }

    this->head_.store(head + to_read, std::memory_order_release);
    return to_read;
  }

  /// Consumer-only: advances head by up to `len` bytes (used to shed excess latency on clock drift).
  size_t skip(size_t len) {
    if (this->buffer_ == nullptr || len == 0) {
      return 0;
    }
    const size_t head = this->head_.load(std::memory_order_relaxed);
    const size_t tail = this->tail_.load(std::memory_order_acquire);
    const size_t avail = tail - head;
    const size_t to_skip = std::min(len, avail);
    if (to_skip > 0) {
      this->head_.store(head + to_skip, std::memory_order_release);
    }
    return to_skip;
  }

  /// Consumer-only: safely discards all currently queued bytes without racing the producer.
  void clear() {
    const size_t tail = this->tail_.load(std::memory_order_acquire);
    this->head_.store(tail, std::memory_order_release);
  }

  void record_underrun() { this->underrun_count_.fetch_add(1, std::memory_order_relaxed); }

  size_t available() const {
    const size_t tail = this->tail_.load(std::memory_order_acquire);
    const size_t head = this->head_.load(std::memory_order_acquire);
    return tail - head;
  }

  size_t capacity() const { return this->capacity_; }
  bool is_allocated() const { return this->buffer_ != nullptr; }
  bool allocated_in_psram() const { return this->in_psram_; }

  float fill_percent() const {
    if (this->capacity_ == 0) {
      return 0.0f;
    }
    return (static_cast<float>(this->available()) * 100.0f) / static_cast<float>(this->capacity_);
  }

  uint32_t overflow_count() const { return this->overflow_count_.load(std::memory_order_relaxed); }
  uint32_t underrun_count() const { return this->underrun_count_.load(std::memory_order_relaxed); }

 private:
  uint8_t *buffer_{nullptr};
  size_t capacity_{0};
  size_t mask_{0};
  bool in_psram_{false};

  alignas(sizeof(size_t)) std::atomic<size_t> head_{0};
  alignas(sizeof(size_t)) std::atomic<size_t> tail_{0};
  std::atomic<uint32_t> overflow_count_{0};
  std::atomic<uint32_t> underrun_count_{0};
};

}  // namespace esphome::bt_a2dp_sink
