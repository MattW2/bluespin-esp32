#include "test_harness.h"

#include "audio_ring.h"
#include "resampler.h"
#include "volume_utils.h"

#include <cmath>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using esphome::bt_a2dp_sink::AudioRingBuffer;
using esphome::bt_a2dp_sink::avrcp_to_percent;
using esphome::bt_a2dp_sink::kAvrcpMaxVolume;
using esphome::bt_a2dp_sink::percent_to_avrcp;
using esphome::bt_a2dp_sink::StereoLinearResampler;

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST_CASE("AudioRingBuffer: power-of-two allocation and basic write/read") {
  AudioRingBuffer ring;
  REQUIRE(ring.allocate(100));
  CHECK(ring.capacity() == 128);
  CHECK(ring.available() == 0);
  CHECK(ring.fill_percent() == 0.0f);

  std::vector<uint8_t> src(80);
  std::iota(src.begin(), src.end(), static_cast<uint8_t>(1));
  CHECK(ring.write(src.data(), src.size()));
  CHECK(ring.available() == 80);

  std::vector<uint8_t> dst(80, 0);
  CHECK(ring.read(dst.data(), dst.size()) == 80);
  CHECK(dst == src);
  CHECK(ring.available() == 0);
}

TEST_CASE("AudioRingBuffer: wrap-around and all-or-nothing overflow drop") {
  AudioRingBuffer ring;
  REQUIRE(ring.allocate(64));
  CHECK(ring.capacity() == 64);

  std::vector<uint8_t> a(48, 0xAA);
  CHECK(ring.write(a.data(), a.size()));

  std::vector<uint8_t> out(32, 0);
  CHECK(ring.read(out.data(), out.size()) == 32);
  CHECK(ring.available() == 16);

  // Write 40 bytes across the end-of-buffer boundary (16 + 40 = 56 <= 64).
  std::vector<uint8_t> b(40);
  for (size_t i = 0; i < b.size(); ++i) {
    b[i] = static_cast<uint8_t>(i + 10);
  }
  CHECK(ring.write(b.data(), b.size()));
  CHECK(ring.available() == 56);

  // Next 16-byte write must fail all-or-nothing (only 8 bytes free) and increment overflow count.
  std::vector<uint8_t> too_big(16, 0x55);
  CHECK(!ring.write(too_big.data(), too_big.size()));
  CHECK(ring.overflow_count() == 1);
  CHECK(ring.available() == 56);

  // Drain remaining 16 bytes of `a` then 40 bytes of `b`.
  std::vector<uint8_t> tail_a(16, 0);
  CHECK(ring.read(tail_a.data(), tail_a.size()) == 16);
  for (uint8_t v : tail_a) {
    CHECK(v == 0xAA);
  }

  std::vector<uint8_t> read_b(40, 0);
  CHECK(ring.read(read_b.data(), read_b.size()) == 40);
  CHECK(read_b == b);
  CHECK(ring.available() == 0);
}

TEST_CASE("AudioRingBuffer: skip, clear, and underrun accounting") {
  AudioRingBuffer ring;
  REQUIRE(ring.allocate(128));

  std::vector<uint8_t> data(100, 0x7F);
  CHECK(ring.write(data.data(), data.size()));
  CHECK(ring.skip(36) == 36);
  CHECK(ring.available() == 64);

  ring.clear();
  CHECK(ring.available() == 0);

  ring.record_underrun();
  ring.record_underrun();
  CHECK(ring.underrun_count() == 2);
}

TEST_CASE("AudioRingBuffer: concurrent SPSC producer/consumer integrity") {
  AudioRingBuffer ring;
  REQUIRE(ring.allocate(4096));

  constexpr size_t kTotalBytes = 256 * 1024;
  constexpr size_t kWriteChunk = 256;
  constexpr size_t kReadChunk = 192;

  std::vector<uint8_t> received;
  received.reserve(kTotalBytes);

  std::thread producer([&]() {
    std::vector<uint8_t> buf(kWriteChunk);
    size_t sent = 0;
    while (sent < kTotalBytes) {
      const size_t n = std::min(kWriteChunk, kTotalBytes - sent);
      for (size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<uint8_t>((sent + i) & 0xFFU);
      }
      while (!ring.write(buf.data(), n)) {
        std::this_thread::yield();
      }
      sent += n;
    }
  });

  std::thread consumer([&]() {
    std::vector<uint8_t> buf(kReadChunk);
    while (received.size() < kTotalBytes) {
      const size_t got = ring.read(buf.data(), buf.size());
      if (got == 0) {
        std::this_thread::yield();
        continue;
      }
      received.insert(received.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
  });

  producer.join();
  consumer.join();

  REQUIRE(received.size() == kTotalBytes);
  bool all_match = true;
  for (size_t i = 0; i < kTotalBytes; ++i) {
    if (received[i] != static_cast<uint8_t>(i & 0xFFU)) {
      all_match = false;
      break;
    }
  }
  CHECK(all_match);
}

TEST_CASE("StereoLinearResampler: 44.1 kHz passthrough") {
  StereoLinearResampler rs;
  rs.configure(44100, 44100);
  CHECK(rs.is_passthrough());

  std::vector<int16_t> in = {100, -100, 200, -200, 300, -300, 400, -400};
  std::vector<int16_t> out(8, 0);
  CHECK(rs.process(in.data(), 4, out.data(), 4) == 4);
  CHECK(out == in);
}

TEST_CASE("StereoLinearResampler: 48 kHz -> 44.1 kHz frame count, DC, and sine fidelity") {
  StereoLinearResampler rs;
  rs.configure(48000, 44100);
  CHECK(!rs.is_passthrough());

  // 1 second of 48 kHz stereo fed in 128-frame SBC-sized packets -> ~44100 frames out.
  constexpr size_t kTotalInFrames = 48000;
  constexpr size_t kPacketFrames = 128;

  std::vector<int16_t> in(kTotalInFrames * 2);
  for (size_t i = 0; i < kTotalInFrames; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    in[i * 2] = static_cast<int16_t>(std::lround(16000.0 * std::sin(2.0 * kPi * 440.0 * t)));
    in[i * 2 + 1] = 12345;  // Right channel constant DC
  }

  std::vector<int16_t> out;
  out.reserve(45000 * 2);
  std::vector<int16_t> chunk_out(rs.max_output_frames(kPacketFrames) * 2);

  for (size_t pos = 0; pos < kTotalInFrames; pos += kPacketFrames) {
    const size_t n = std::min(kPacketFrames, kTotalInFrames - pos);
    const size_t produced = rs.process(&in[pos * 2], n, chunk_out.data(), chunk_out.size() / 2);
    out.insert(out.end(), chunk_out.begin(), chunk_out.begin() + static_cast<std::ptrdiff_t>(produced * 2));
  }

  const size_t out_frames = out.size() / 2;
  // Should be within +-2 frames of 44100 over 1 full second.
  CHECK(out_frames >= 44098);
  CHECK(out_frames <= 44102);

  // Verify right channel DC stays exact and left channel 440 Hz sine tracks within linear-interp bound.
  double max_sine_err = 0.0;
  bool dc_exact = true;
  // Skip frame 0 (initial boundary anchor) and check steady state.
  for (size_t i = 2; i < out_frames; ++i) {
    if (out[i * 2 + 1] != 12345) {
      dc_exact = false;
    }
    // Note: virtual index 0 was in[0] (t=0), index 1 was in[0]..in[1], so output lags by 1 input sample (1/48000 s).
    const double t = (static_cast<double>(i) / 44100.0) - (1.0 / 48000.0);
    const double expected = 16000.0 * std::sin(2.0 * kPi * 440.0 * std::max(0.0, t));
    max_sine_err = std::max(max_sine_err, std::abs(static_cast<double>(out[i * 2]) - expected));
  }
  CHECK(dc_exact);
  CHECK(max_sine_err < 35.0);  // < 0.22% of 16000 peak amplitude
}

TEST_CASE("volume_utils: AVRCP 0..127 <-> 0..100% conversions") {
  CHECK(avrcp_to_percent(0) == 0);
  CHECK(avrcp_to_percent(kAvrcpMaxVolume) == 100);
  CHECK(avrcp_to_percent(200) == 100);  // Clamps above 127

  CHECK(percent_to_avrcp(0.0f) == 0);
  CHECK(percent_to_avrcp(-5.0f) == 0);
  CHECK(percent_to_avrcp(100.0f) == kAvrcpMaxVolume);
  CHECK(percent_to_avrcp(150.0f) == kAvrcpMaxVolume);

  // Every integer percentage 0..100 should round-trip within +-1%.
  for (int pct = 0; pct <= 100; ++pct) {
    const uint8_t avrcp = percent_to_avrcp(static_cast<float>(pct));
    const int back = avrcp_to_percent(avrcp);
    CHECK(std::abs(back - pct) <= 1);
  }
}

int main() { return test_harness::run_all(); }
