#pragma once
#include "protocol.hpp"
#include "report_queue.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sensor_hid {
inline std::array<float, 3> acceleration_g(std::array<float, 3> meters_per_second_squared) {
    for (auto &value : meters_per_second_squared)
        value /= 9.80665f;
    return meters_per_second_squared;
}
#pragma pack(push, 1)
struct FeatureReport {
    uint8_t id{}, connection{3}, reporting{1}, power{2}, state{5};
    uint32_t sensitivity{}, interval{16}, minimum_interval{4};
};
struct InputReport {
    uint8_t id{}, state{5}, event{4};
    int32_t axes[3]{};
};
#pragma pack(pop)
static_assert(sizeof(FeatureReport) == 17 && sizeof(InputReport) == 15);
struct Channel {
    FeatureReport feature;
    InputReport input;
    int32_t last_axes[3]{};
    uint64_t last_sent{};
    bool pending{}, first{true};
    switch2::ReportQueue<InputReport, 64> reports;

    void initialize(uint8_t id) {
        *this = Channel{};
        feature.id = input.id = id;
    }
    bool set(const FeatureReport &requested) {
        if (requested.reporting < 1 || requested.reporting > 6 || requested.power < 1 ||
            requested.power > 6 || requested.sensitivity > 0x7fffffff)
            return false;
        feature.reporting = requested.reporting;
        feature.power = requested.power;
        feature.sensitivity = requested.sensitivity;
        feature.interval = std::clamp(requested.interval, feature.minimum_interval, 1000u);
        first = true;
        pending = false;
        reports.clear();
        return true;
    }
    void update(const std::array<float, 3> &axes, uint8_t state, uint64_t now) {
        const bool state_changed = feature.state != state;
        if (state_changed) {
            reports.clear();
            pending = false;
        }
        feature.state = input.state = state;
        if (state == 2)
            for (size_t i = 0; i < 3; ++i)
                input.axes[i] = int32_t(std::lround(axes[i] * 10000.0f));
        // No events in sleep/off or the two "no events" reporting states.
        if (feature.reporting == 1 || feature.reporting == 4 || feature.power >= 4)
            return;
        bool changed = first || state_changed;
        for (size_t i = 0; i < 3; ++i)
            changed |= std::abs(int64_t(input.axes[i]) - last_axes[i]) >= feature.sensitivity;
        const bool all = feature.reporting == 2 || feature.reporting == 5;
        // At the minimum interval the physical stream already limits the rate.
        // Applying another wall-clock gate would drop samples due to delivery jitter.
        const bool due =
            feature.interval == feature.minimum_interval || now - last_sent >= feature.interval;
        if (state_changed || ((all || changed) && due)) {
            input.event = state_changed ? 2 : 4;
            reports.push(input);
            pending = true;
        }
    }
    void sent(uint64_t now) {
        if (!reports.empty()) {
            const auto &report = reports.front();
            std::copy(std::begin(report.axes), std::end(report.axes), last_axes);
            reports.pop();
        }
        pending = !reports.empty();
        first = false;
        last_sent = now;
    }
};
} // namespace sensor_hid
