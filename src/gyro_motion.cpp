#include "gyro_motion.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace switch2 {
double GyroMotion::dot(const Vector &a, const Vector &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
GyroMotion::Vector GyroMotion::cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
GyroMotion::Vector GyroMotion::unit(Vector value) {
    const double length = std::sqrt(dot(value, value));
    if (length < 1e-9 || !std::isfinite(length)) return {0, 0, 1};
    for (auto &axis : value) axis /= length;
    return value;
}
bool GyroMotion::valid(const Motion &sample) {
    if (!sample.ready || !sample.sample_time_100ns) return false;
    for (float value : sample.angular_velocity) if (!std::isfinite(value)) return false;
    for (float value : sample.acceleration) if (!std::isfinite(value)) return false;
    return true;
}
void GyroMotion::calibrate(const Motion &sample) {
    if (!valid(sample) || sample.sample_time_100ns <= calibration_time_) return;
    Vector rate{}, acceleration{};
    for (unsigned i = 0; i < 3; ++i) {
        rate[i] = sample.angular_velocity[i];
        acceleration[i] = sample.acceleration[i];
    }
    double magnitude = std::sqrt(dot(acceleration, acceleration));
    if (magnitude > 6.9) { for (auto &axis : acceleration) axis /= 9.80665; magnitude /= 9.80665; }
    const bool gap = calibration_time_ && sample.sample_time_100ns - calibration_time_ > 1000000;
    calibration_time_ = sample.sample_time_100ns;
    if (gap || dot(rate, rate) > 9 || magnitude < 0.9 || magnitude > 1.1) {
        calibration_count_ = 0;
        return;
    }
    if (!calibration_count_) {
        calibration_mean_ = calibration_m2_ = {};
        acceleration_mean_ = acceleration;
    }
    for (unsigned i = 0; i < 3; ++i)
        if (std::abs(acceleration[i] - acceleration_mean_[i]) > 0.03) {
            calibration_count_ = 0;
            return;
        }
    ++calibration_count_;
    for (unsigned i = 0; i < 3; ++i) {
        const double delta = rate[i] - calibration_mean_[i];
        calibration_mean_[i] += delta / calibration_count_;
        calibration_m2_[i] += delta * (rate[i] - calibration_mean_[i]);
        acceleration_mean_[i] += (acceleration[i] - acceleration_mean_[i]) / calibration_count_;
    }
    if (calibration_count_ >= 40) {
        bool stable = true;
        for (double value : calibration_m2_) stable &= value / (calibration_count_ - 1) < 0.06;
        if (stable) bias_ = calibration_mean_;
        if (!stable || calibration_count_ >= 200) calibration_count_ = 0;
    }
}
void GyroMotion::reset() {
    started_ = false;
    previous_time_ = 0;
    calibration_count_ = 0;
}
void GyroMotion::begin(const Motion &sample) {
    if (!valid(sample)) return;
    gravity_ = unit({sample.acceleration[0], sample.acceleration[1], sample.acceleration[2]});
    previous_time_ = sample.sample_time_100ns;
    calibration_count_ = 0;
    started_ = true;
}
std::optional<GyroDelta> GyroMotion::update(const Motion &sample) {
    if (!valid(sample) || !started_ || sample.sample_time_100ns <= previous_time_) return {};
    const auto elapsed = sample.sample_time_100ns - previous_time_;
    previous_time_ = sample.sample_time_100ns;
    const auto interval = sample.sample_interval_100ns ? sample.sample_interval_100ns : elapsed;
    if (elapsed > 1000000 || interval > 1000000) { started_ = false; return {}; }
    const double seconds = double(interval) / 10000000.0;
    Vector rate{};
    for (unsigned i = 0; i < 3; ++i) rate[i] = sample.angular_velocity[i] - bias_[i];
    const auto previous_gravity = gravity_;
    const double speed = std::sqrt(dot(rate, rate));
    if (speed > 1e-9) {
        Vector axis = rate;
        for (auto &value : axis) value /= speed;
        const double angle = speed * std::numbers::pi / 180 * seconds;
        const auto perpendicular = cross(axis, gravity_);
        const double parallel = dot(axis, gravity_);
        for (unsigned i = 0; i < 3; ++i)
            gravity_[i] = previous_gravity[i] * std::cos(angle) - perpendicular[i] * std::sin(angle) +
                          axis[i] * parallel * (1 - std::cos(angle));
    }
    // Predict gravity through a fast roll, then gently correct its direction.
    // This changes the axes of future movement; it never moves the cursor by itself.
    Vector acceleration{sample.acceleration[0], sample.acceleration[1], sample.acceleration[2]};
    double magnitude = std::sqrt(dot(acceleration, acceleration));
    if (magnitude > 6.9) magnitude /= 9.80665;
    if (magnitude > 0.9 && magnitude < 1.1) {
        const auto measured = unit(acceleration);
        const double weight = 1 - std::exp(-seconds / 0.25);
        for (unsigned i = 0; i < 3; ++i) gravity_[i] += weight * (measured[i] - gravity_[i]);
    }
    gravity_ = unit(gravity_);
    Vector middle{};
    for (unsigned i = 0; i < 3; ++i) middle[i] = previous_gravity[i] + gravity_[i];
    middle = unit(middle);
    const double horizontal = -dot(rate, middle);
    const double pitch_length = std::hypot(middle[0], middle[2]);
    const double vertical = pitch_length > 0.15 ? -(rate[0] * middle[2] - rate[2] * middle[0]) / pitch_length : -rate[0];
    // Reject residual stationary noise without shrinking real movement. Unlike
    // the old soft dead zone, equal turns above this floor have equal travel.
    if (std::hypot(horizontal, vertical) <= 0.25) return GyroDelta{};
    return GyroDelta{horizontal * seconds * GyroCountsPerDegree, vertical * seconds * GyroCountsPerDegree};
}
} // namespace switch2
