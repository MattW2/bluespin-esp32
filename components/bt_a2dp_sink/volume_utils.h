#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::bt_a2dp_sink {

inline constexpr uint8_t kAvrcpMaxVolume = 0x7FU;  // 127 per AVRCP v1.4+ spec

/// Converts AVRCP absolute volume (0..127) to rounded percentage (0..100).
inline constexpr uint8_t avrcp_to_percent(uint8_t avrcp_volume) {
  const uint8_t clamped = avrcp_volume > kAvrcpMaxVolume ? kAvrcpMaxVolume : avrcp_volume;
  return static_cast<uint8_t>((static_cast<uint32_t>(clamped) * 100U + 63U) / kAvrcpMaxVolume);
}

/// Converts percentage (0.0..100.0) to rounded AVRCP absolute volume (0..127).
inline uint8_t percent_to_avrcp(float percent) {
  if (!std::isfinite(percent) || percent <= 0.0f) {
    return 0;
  }
  if (percent >= 100.0f) {
    return kAvrcpMaxVolume;
  }
  const float scaled = (percent * static_cast<float>(kAvrcpMaxVolume)) / 100.0f;
  return static_cast<uint8_t>(std::lround(scaled));
}

}  // namespace esphome::bt_a2dp_sink
