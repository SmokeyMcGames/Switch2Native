#include "motion_orientation.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace switch2 {
namespace {
using Quaternion = std::array<double, 4>;
using Vector = std::array<double, 3>;
Quaternion multiply(const Quaternion &a, const Quaternion &b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}
void normalize(Quaternion &q) {
    const auto length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (auto &value : q)
        value /= length;
}
} // namespace
void MotionOrientation::update(const Motion &sample) {
    if (!sample.ready || !sample.sample_time_100ns)
        return;
    for (auto value : sample.angular_velocity)
        if (!std::isfinite(value))
            return;
    for (auto value : sample.acceleration)
        if (!std::isfinite(value))
            return;
    const auto &acceleration = sample.acceleration;
    const double magnitude = std::hypot(acceleration[0], acceleration[1], acceleration[2]);
    const bool gravity_valid = magnitude > 0.85 * 9.80665 && magnitude < 1.15 * 9.80665;
    Vector measured{};
    if (gravity_valid)
        for (unsigned i = 0; i < 3; ++i)
            measured[i] = acceleration[i] / magnitude;
    if (!previous_time_) {
        if (gravity_valid) {
            rotation_ = measured[2] < -0.9999
                            ? Quaternion{0, 1, 0, 0}
                            : Quaternion{1 + measured[2], measured[1], -measured[0], 0};
            normalize(rotation_);
        }
        previous_time_ = sample.sample_time_100ns;
        return;
    }
    if (sample.sample_time_100ns <= previous_time_)
        return;
    const auto elapsed = sample.sample_time_100ns - previous_time_;
    previous_time_ = sample.sample_time_100ns;
    if (elapsed > 1000000)
        return; // Never integrate a missing interval as a giant turn.
    const double seconds =
        (sample.sample_interval_100ns ? sample.sample_interval_100ns : elapsed) * 1e-7;
    if (seconds <= 0 || seconds > 0.1)
        return;
    Vector rate;
    for (unsigned i = 0; i < 3; ++i)
        rate[i] = sample.angular_velocity[i] * std::numbers::pi / 180;
    if (gravity_valid) {
        const auto &[w, x, y, z] = rotation_;
        const Vector predicted{2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
        const Vector error{measured[1] * predicted[2] - measured[2] * predicted[1],
                           measured[2] * predicted[0] - measured[0] * predicted[2],
                           measured[0] * predicted[1] - measured[1] * predicted[0]};
        for (unsigned i = 0; i < 3; ++i)
            rate[i] += 1.5 * error[i];
    }
    const double speed = std::hypot(rate[0], rate[1], rate[2]);
    if (speed > 1e-12) {
        const double half_angle = speed * seconds / 2, scale = std::sin(half_angle) / speed;
        rotation_ = multiply(
            rotation_, {std::cos(half_angle), rate[0] * scale, rate[1] * scale, rate[2] * scale});
        normalize(rotation_);
    }
}
std::array<float, 4> MotionOrientation::quaternion() const {
    return {float(rotation_[0]), float(rotation_[1]), float(rotation_[2]), float(rotation_[3])};
}
} // namespace switch2
