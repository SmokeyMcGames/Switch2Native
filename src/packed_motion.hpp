#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace switch2 {
struct PackedMotionConfig {
    double samples_per_second{960.0};
    double acceleration_range{78.4532};
    double angular_velocity_range{34.906585};
    double dense_acceleration_range{19.6133};
    double dense_angular_velocity_range{8.726646};
};
// Data section of the controller's 0x11/0x03 response (version 1).
std::optional<PackedMotionConfig> parse_packed_motion_config(std::span<const uint8_t> data);

struct PackedMotionHeader {
    uint16_t end_tick{}, duration{};
    uint8_t pattern{}, flags{};
    size_t bytes{}, sample_count{};
};
std::optional<PackedMotionHeader> packed_motion_header(std::span<const uint8_t> payload);

struct PackedMotionSample {
    uint16_t end_tick{}, duration{}, encoded_duration{};
    bool gyro_valid{}, recovered_gap{};
    // After packet loss, only the gyro's duration can cover the missing interval.
    // Acceleration still describes encoded_duration, not the recovered gap.
    std::array<double, 3> acceleration{}, angular_velocity{}; // m/s^2 and radians/s
    std::array<double, 4> orientation{}; // Internal integrated orientation, w/x/y/z.
};
struct PackedMotionFrame {
    PackedMotionHeader header;
    std::array<PackedMotionSample, 3> samples{};
};

// Decodes the controller's accumulated orientation. This is not an absolute
// heading estimate; factory gyro bias still has to be removed from its rates.
class PackedMotionDecoder {
  public:
    explicit PackedMotionDecoder(PackedMotionConfig config = {}) : config_(config) {}
    std::optional<PackedMotionFrame> decode(std::span<const uint8_t> payload, uint64_t host_us);
    void reset();

  private:
    PackedMotionConfig config_;
    std::array<double, 4> previous_orientation_{1, 0, 0, 0};
    uint16_t previous_tick_{};
    uint64_t previous_host_us_{};
    bool have_orientation_{};
};
} // namespace switch2
