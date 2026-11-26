#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::bt_a2dp_sink {

/**
 * Lightweight stateful stereo 16-bit PCM linear resampler (e.g. 48 kHz -> 44.1 kHz).
 *
 * Uses Q16.16 fixed-point phase accumulation and preserves boundary samples across calls
 * so packetized A2DP SBC frames resample seamlessly without clicks.
 */
class StereoLinearResampler {
 public:
  StereoLinearResampler() { this->configure(44100, 44100); }

  void configure(uint32_t in_rate, uint32_t out_rate) {
    if (in_rate == 0 || out_rate == 0) {
      in_rate = 44100;
      out_rate = 44100;
    }
    if (this->in_rate_ == in_rate && this->out_rate_ == out_rate) {
      return;
    }
    this->in_rate_ = in_rate;
    this->out_rate_ = out_rate;
    this->step_q32_ = (static_cast<uint64_t>(in_rate) << 32) / static_cast<uint64_t>(out_rate);
    this->reset();
  }

  void reset() {
    this->phase_q32_ = 0;
    this->has_prev_ = false;
    this->prev_l_ = 0;
    this->prev_r_ = 0;
  }

  uint32_t input_rate() const { return this->in_rate_; }
  uint32_t output_rate() const { return this->out_rate_; }
  bool is_passthrough() const { return this->in_rate_ == this->out_rate_; }

  /// Upper bound on output stereo frames produced by `in_frames` input stereo frames.
  size_t max_output_frames(size_t in_frames) const {
    if (in_frames == 0) {
      return 0;
    }
    if (this->is_passthrough()) {
      return in_frames;
    }
    return static_cast<size_t>((static_cast<uint64_t>(in_frames) * this->out_rate_ + this->in_rate_ - 1) /
                               this->in_rate_) +
           2;
  }

  /**
   * Resamples interleaved stereo int16 frames (`in_frames` count, i.e., `in_frames * 2` int16 samples)
   * into `out` (capacity `max_out_frames` stereo frames).
   *
   * @return Number of stereo frames written to `out`.
   */
  size_t process(const int16_t *in, size_t in_frames, int16_t *out, size_t max_out_frames) {
    if (in == nullptr || out == nullptr || in_frames == 0 || max_out_frames == 0) {
      return 0;
    }

    if (this->is_passthrough()) {
      const size_t n = std::min(in_frames, max_out_frames);
      std::memcpy(out, in, n * 2 * sizeof(int16_t));
      this->prev_l_ = in[(n - 1) * 2];
      this->prev_r_ = in[(n - 1) * 2 + 1];
      this->has_prev_ = true;
      return n;
    }

    if (!this->has_prev_) {
      this->prev_l_ = in[0];
      this->prev_r_ = in[1];
      this->has_prev_ = true;
      this->phase_q32_ = 0;
    }

    // Index 0 in our virtual window is (prev_l_, prev_r_); index k (1..in_frames) is in[(k-1)*2 ..].
    // Valid interpolation requires idx + 1 <= in_frames, i.e., idx < in_frames.
    size_t out_written = 0;
    const uint64_t limit_q32 = static_cast<uint64_t>(in_frames) << 32;

    while (this->phase_q32_ < limit_q32 && out_written < max_out_frames) {
      const size_t idx = static_cast<size_t>(this->phase_q32_ >> 32);
      const int32_t frac = static_cast<int32_t>((this->phase_q32_ >> 16) & 0xFFFFU);

      int32_t l0, r0;
      if (idx == 0) {
        l0 = this->prev_l_;
        r0 = this->prev_r_;
      } else {
        l0 = in[(idx - 1) * 2];
        r0 = in[(idx - 1) * 2 + 1];
      }
      const int32_t l1 = in[idx * 2];
      const int32_t r1 = in[idx * 2 + 1];

      const int32_t l = l0 + (((l1 - l0) * frac) >> 16);
      const int32_t r = r0 + (((r1 - r0) * frac) >> 16);

      out[out_written * 2] = static_cast<int16_t>(std::clamp<int32_t>(l, -32768, 32767));
      out[out_written * 2 + 1] = static_cast<int16_t>(std::clamp<int32_t>(r, -32768, 32767));
      ++out_written;

      this->phase_q32_ += this->step_q32_;
    }

    // Shift window forward by `in_frames` so the remaining fractional offset carries into the next call.
    if (this->phase_q32_ >= limit_q32) {
      this->phase_q32_ -= limit_q32;
    } else {
      // Output buffer filled before consuming all input; advance to end of input cleanly.
      this->phase_q32_ = 0;
    }
    this->prev_l_ = in[(in_frames - 1) * 2];
    this->prev_r_ = in[(in_frames - 1) * 2 + 1];

    return out_written;
  }

 private:
  uint32_t in_rate_{44100};
  uint32_t out_rate_{44100};
  uint64_t step_q32_{1ULL << 32};
  uint64_t phase_q32_{0};
  int16_t prev_l_{0};
  int16_t prev_r_{0};
  bool has_prev_{false};
};

}  // namespace esphome::bt_a2dp_sink
