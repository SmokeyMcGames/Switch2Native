#pragma once
#include "packed_motion.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace switch2 {
// Preserve device tick spacing within a batch. Slowly track the earliest radio
// arrivals in half-second windows so oscillator error cannot accumulate seconds
// of timestamp lag, while individual delayed packets do not move the clock.
class PackedSampleClock {
  public:
    explicit PackedSampleClock(double frequency) : frequency_(frequency) {}
    uint64_t end_time(uint16_t tick, uint64_t host_us, uint64_t arrival_100ns) {
        if (!valid_ || host_us < previous_host_ || host_us - previous_host_ >= 2000000) {
            anchor_ = arrival_100ns;
            ticks_ = 0;
            valid_ = true;
            target_anchor_ = window_minimum_ = anchor_;
            window_host_ = host_us;
        } else {
            const auto delta = uint16_t((tick - previous_tick_) & 4095);
            ticks_ += delta;
            const auto arrival_anchor = arrival_100ns - ticks_to_time(ticks_);
            window_minimum_ = std::min(window_minimum_, arrival_anchor);
            if (host_us - window_host_ >= 500000) {
                target_anchor_ = window_minimum_;
                window_minimum_ = arrival_anchor;
                window_host_ = host_us;
            }
            // Slew by at most 2% of elapsed sensor time. This corrects measured
            // sub-percent crystal skew without sudden periodic timestamp jumps.
            const auto limit = ticks_to_time(delta) / 50;
            if (target_anchor_ > anchor_)
                anchor_ += std::min(target_anchor_ - anchor_, limit);
            else
                anchor_ -= std::min(anchor_ - target_anchor_, limit);
            anchor_ = std::min(anchor_, arrival_anchor);
        }
        previous_tick_ = tick;
        previous_host_ = host_us;
        return anchor_ + ticks_to_time(ticks_);
    }
    uint64_t ticks_to_time(uint64_t ticks) const {
        return uint64_t(std::llround(double(ticks) * 10000000.0 / frequency_));
    }
  private:
    double frequency_;
    uint64_t anchor_{}, ticks_{}, previous_host_{};
    uint64_t target_anchor_{}, window_minimum_{}, window_host_{};
    uint16_t previous_tick_{};
    bool valid_{};
};

struct MotionBatch {
    std::array<Motion, 3> samples{};
    size_t count{};
};

class PackedMotionProcessor {
  public:
    PackedMotionProcessor(PackedMotionConfig config, std::array<float, 3> bias)
        : decoder_(config), clock_(config.samples_per_second), bias_(bias) {}
    std::optional<MotionBatch> process(std::span<const uint8_t> payload, uint64_t host_us,
                                       uint64_t arrival_100ns) {
        const auto frame = decoder_.decode(payload, host_us);
        if (!frame) return std::nullopt;
        MotionBatch batch;
        if (!frame->header.duration) return batch;
        const auto end = clock_.end_time(frame->header.end_tick, host_us, arrival_100ns);
        for (size_t i = 0; i < frame->header.sample_count; ++i) {
            const auto &source = frame->samples[i];
            if (!source.gyro_valid) continue;
            Motion sample;
            const auto age = uint16_t((frame->header.end_tick - source.end_tick) & 4095);
            sample.sample_time_100ns = end - clock_.ticks_to_time(age);
            sample.sample_interval_100ns = uint32_t(clock_.ticks_to_time(source.duration));
            if (sample.sample_time_100ns <= last_sample_) continue;
            sample.ready = true;
            for (unsigned axis = 0; axis < 3; ++axis) {
                sample.acceleration[axis] = float(source.acceleration[axis]);
                sample.angular_velocity[axis] = float((source.angular_velocity[axis] - bias_[axis]) *
                                                       180.0 / std::numbers::pi);
            }
            last_sample_ = sample.sample_time_100ns;
            batch.samples[batch.count++] = sample;
        }
        return batch;
    }
  private:
    PackedMotionDecoder decoder_;
    PackedSampleClock clock_;
    std::array<float, 3> bias_;
    uint64_t last_sample_{};
};
} // namespace switch2
