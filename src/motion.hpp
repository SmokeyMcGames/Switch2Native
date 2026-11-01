#pragma once
#include "protocol.hpp"
#include <chrono>

namespace switch2 {
// Factory gyro offsets use radians/s. Keep them in the controller's right-handed
// frame: X right, Y toward the shoulders, Z out of the face buttons.
std::optional<std::array<float, 3>> parse_gyro_bias(std::span<const uint8_t> factory);

class MotionProcessor {
  public:
    explicit MotionProcessor(std::array<float, 3> bias) : bias_(bias) {}
    Motion process(const RawMotion &raw, std::chrono::steady_clock::time_point now);

  private:
    std::array<float, 3> bias_;
    std::chrono::steady_clock::time_point started_{};
    uint32_t first_timestamp_{}, last_timestamp_{};
    bool started_valid_{}, ready_{};
    float gyro_range_{};
};
} // namespace switch2
