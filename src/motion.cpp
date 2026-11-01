#include "motion.hpp"
#include <cmath>
#include <cstring>
#include <numbers>

namespace switch2 {
std::optional<std::array<float, 3>> parse_gyro_bias(std::span<const uint8_t> factory) {
    if (factory.size() < 16)
        return std::nullopt;
    std::array<float, 3> bias;
    std::memcpy(bias.data(), factory.data() + 4, sizeof(bias));
    for (float value : bias)
        if (!std::isfinite(value) || std::abs(value) > 1.0f)
            return std::nullopt;
    return bias;
}
Motion MotionProcessor::process(const RawMotion &raw, std::chrono::steady_clock::time_point now) {
    Motion result;
    if (!raw.timestamp || (started_valid_ && raw.timestamp == last_timestamp_))
        return result;
    last_timestamp_ = raw.timestamp;
    if (!started_valid_) {
        started_valid_ = true;
        started_ = now;
        first_timestamp_ = raw.timestamp;
        return result;
    }
    if (!ready_) {
        const double seconds = std::chrono::duration<double>(now - started_).count();
        if (seconds < 1.0)
            return result;
        const auto ticks = uint32_t(raw.timestamp - first_timestamp_);
        const double frequency = ticks / seconds;
        // SDL identifies two IMU modes by their clock. Measure elapsed time rather
        // than assuming USB's 250 Hz, so dropped packets and Bluetooth also work.
        if (frequency <= 0)
            return result;
        gyro_range_ = frequency >= 900000 && frequency <= 1100000 ? 34.8f : 40.0f;
        ready_ = true;
    }
    result.ready = true;
    constexpr float radians_to_degrees = 180.0f / std::numbers::pi_v<float>;
    for (size_t i = 0; i < 3; ++i) {
        result.acceleration[i] = raw.acceleration[i] * (8.0f * 9.80665f / 32767.0f);
        result.angular_velocity[i] =
            (raw.angular_velocity[i] * gyro_range_ / 32767.0f - bias_[i]) * radians_to_degrees;
    }
    return result;
}
} // namespace switch2
