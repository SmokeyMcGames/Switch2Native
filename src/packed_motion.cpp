#include "packed_motion.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace switch2 {
namespace {
using Quaternion = std::array<double, 4>;

bool valid_config(const PackedMotionConfig &config) {
    if (!std::isfinite(config.samples_per_second) || config.samples_per_second < 100 ||
        config.samples_per_second > 10000)
        return false;
    for (double range : {config.acceleration_range, config.angular_velocity_range,
                         config.dense_acceleration_range, config.dense_angular_velocity_range})
        if (!std::isfinite(range) || range <= 0 || range > 10000)
            return false;
    return config.dense_acceleration_range <= config.acceleration_range &&
           config.dense_angular_velocity_range <= config.angular_velocity_range;
}

class Bits {
  public:
    explicit Bits(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint32_t read(unsigned width) {
        if (width > 32 || position_ + width > bytes_.size() * 8)
            throw std::out_of_range("Truncated packed motion field");
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i, ++position_)
            value |= uint32_t((bytes_[position_ / 8] >> (position_ % 8)) & 1) << i;
        return value;
    }
    double signed_fraction(unsigned width) {
        const auto value = uint64_t(read(width));
        const auto sign = uint64_t{1} << (width - 1);
        const auto integer = value & sign ? int64_t(value) - int64_t(sign * 2) : int64_t(value);
        return double(integer) / double(sign);
    }

  private:
    std::span<const uint8_t> bytes_;
    size_t position_{};
};

Quaternion multiply(const Quaternion &a, const Quaternion &b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Quaternion read_orientation(Bits &bits, unsigned budget) {
    const unsigned largest = bits.read(2);
    const unsigned base_width = (budget - 2) / 3;
    const unsigned extra = (budget - 2) % 3;
    Quaternion result{};
    result[largest] = 1;
    double norm_squared = 1;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned width = base_width + unsigned(i < extra);
        const double value = double(bits.read(width)) / double(uint64_t{1} << (width - 1)) - 1;
        result[(largest + i + 1) % 4] = value;
        norm_squared += value * value;
    }
    for (auto &value : result)
        value /= std::sqrt(norm_squared);
    return result;
}

std::array<double, 3> angular_velocity(const Quaternion &previous, const Quaternion &current,
                                       double seconds) {
    const Quaternion inverse{previous[0], -previous[1], -previous[2], -previous[3]};
    auto difference = multiply(inverse, current);
    // q and -q represent the same orientation, including when the omitted
    // largest component changes. Choose the short rotation across that boundary.
    if (difference[0] < 0)
        for (auto &value : difference)
            value = -value;
    const double length = std::sqrt(difference[1] * difference[1] + difference[2] * difference[2] +
                                    difference[3] * difference[3]);
    const double scale =
        length > 1e-12 ? 2 * std::atan2(length, difference[0]) / (length * seconds) : 2 / seconds;
    return {difference[1] * scale, difference[2] * scale, difference[3] * scale};
}

Quaternion integrate(const Quaternion &previous, const std::array<double, 3> &rotation) {
    const double angle = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] +
                                   rotation[2] * rotation[2]);
    const double scale = angle > 1e-12 ? std::sin(angle / 2) / angle : 0.5;
    auto result = multiply(previous, {std::cos(angle / 2), rotation[0] * scale, rotation[1] * scale,
                                      rotation[2] * scale});
    double norm_squared = 0;
    for (auto value : result)
        norm_squared += value * value;
    for (auto &value : result)
        value /= std::sqrt(norm_squared);
    return result;
}

int available_tick(int target, int end) {
    if (target >= end - 19)
        return target;
    // Older controller states are retained every sixteen sensor ticks. Match
    // its deterministic history instead of inventing evenly spaced samples.
    const int newest_sparse = (end - 20) & ~15;
    const int oldest_sparse = newest_sparse - 15 * 16;
    return std::max(oldest_sparse, (target + 15) & ~15);
}
} // namespace

std::optional<PackedMotionConfig> parse_packed_motion_config(std::span<const uint8_t> data) {
    if (data.size() != 29 || data[0] != 1)
        return std::nullopt;
    uint32_t frequency = 0;
    for (unsigned i = 0; i < 4; ++i)
        frequency |= uint32_t(data[i + 1]) << (8 * i);
    PackedMotionConfig config;
    config.samples_per_second = frequency;
    double *ranges[]{&config.acceleration_range, &config.angular_velocity_range,
                     &config.dense_acceleration_range, &config.dense_angular_velocity_range};
    for (size_t i = 0; i < 4; ++i) {
        float value;
        std::memcpy(&value, data.data() + 13 + i * 4, sizeof(value));
        *ranges[i] = value;
    }
    return valid_config(config) ? std::optional(config) : std::nullopt;
}

std::optional<PackedMotionHeader> packed_motion_header(std::span<const uint8_t> payload) {
    if (payload.size() < 4)
        return std::nullopt;
    PackedMotionHeader header;
    header.end_tick = uint16_t(payload[0] | ((payload[1] & 15) << 8));
    header.duration = uint16_t((payload[1] >> 4) | (payload[2] << 4));
    header.pattern = payload[3] & 3;
    header.flags = payload[3] >> 2;
    header.sample_count = header.duration ? (header.pattern == 0   ? 1
                                             : header.pattern == 1 ? 2
                                                                   : 3)
                                          : 0;
    header.bytes = header.duration ? (header.pattern == 0 ? 30 : 40) : 4;
    if (payload.size() < header.bytes || header.duration < header.sample_count)
        return std::nullopt;
    return header;
}

void PackedMotionDecoder::reset() {
    previous_orientation_ = {1, 0, 0, 0};
    previous_tick_ = 0;
    previous_host_us_ = 0;
    have_orientation_ = false;
}

std::optional<PackedMotionFrame> PackedMotionDecoder::decode(std::span<const uint8_t> payload,
                                                             uint64_t host_us) {
    const auto header = packed_motion_header(payload);
    if (!header || !valid_config(config_))
        return std::nullopt;
    PackedMotionFrame frame{*header};
    if (!header->duration)
        return frame;

    const bool previous_valid =
        have_orientation_ && host_us >= previous_host_us_ && host_us - previous_host_us_ < 2000000;
    const auto elapsed = uint16_t((header->end_tick - previous_tick_) & 4095);
    if (previous_valid && (!elapsed || elapsed > 2048))
        return std::nullopt;

    Bits bits(payload.subspan(4, header->bytes - 4));
    const auto density = header->pattern ? bits.read(2) : 0;
    const double gyro_range =
        density & 1 ? config_.dense_angular_velocity_range : config_.angular_velocity_range;
    const double accel_range =
        density & 2 ? config_.dense_acceleration_range : config_.acceleration_range;
    constexpr unsigned gyro_budgets[4][3]{{96, 0, 0}, {72, 66, 0}, {66, 39, 42}, {66, 48, 48}};
    constexpr unsigned accel_widths[4][3]{{32, 0, 0}, {22, 22, 0}, {14, 13, 14}, {14, 13, 14}};
    // The extra epoch keeps sparse-buffer arithmetic positive through wrapping.
    const int end = header->end_tick + 4096;
    int start = end - header->duration;
    Quaternion orientation = previous_orientation_;
    for (size_t i = 0; i < header->sample_count; ++i) {
        const int remaining = int(header->sample_count - i);
        const int target = start + (end - start + remaining - 1) / remaining;
        const int next = remaining == 1 ? end : available_tick(target, end);
        if (next <= start || next > end)
            return std::nullopt;
        auto &sample = frame.samples[i];
        sample.end_tick = uint16_t(next & 4095);
        sample.duration = uint16_t(next - start);
        sample.encoded_duration = sample.duration;
        if (i == 0) {
            sample.orientation = read_orientation(bits, gyro_budgets[header->pattern][i]);
            sample.gyro_valid = previous_valid;
            if (previous_valid) {
                // A missing packet's rotation is recoverable as an interval
                // average. Mark that interval; never emit invented extra frames.
                const int actual_duration = int(sample.duration) + int(elapsed) - header->duration;
                if (actual_duration <= 0)
                    return std::nullopt;
                sample.recovered_gap = elapsed != header->duration;
                sample.duration = uint16_t(actual_duration);
                sample.angular_velocity = angular_velocity(
                    orientation, sample.orientation, sample.duration / config_.samples_per_second);
            }
        } else {
            const double seconds = sample.duration / config_.samples_per_second;
            const double maximum_angle = std::min(gyro_range * seconds, 2 * std::numbers::pi);
            std::array<double, 3> rotation;
            for (size_t axis = 0; axis < 3; ++axis) {
                rotation[axis] =
                    bits.signed_fraction(gyro_budgets[header->pattern][i] / 3) * maximum_angle;
                sample.angular_velocity[axis] = rotation[axis] / seconds;
            }
            sample.orientation = integrate(orientation, rotation);
            sample.gyro_valid = true;
        }
        for (auto &axis : sample.acceleration)
            axis = bits.signed_fraction(accel_widths[header->pattern][i]) * accel_range;
        orientation = sample.orientation;
        start = next;
    }
    if (header->pattern != 3)
        bits.read(16); // Temperature is not exposed until its scaling is validated.
    previous_orientation_ = orientation;
    previous_tick_ = header->end_tick;
    previous_host_us_ = host_us;
    have_orientation_ = true;
    return frame;
}
} // namespace switch2
