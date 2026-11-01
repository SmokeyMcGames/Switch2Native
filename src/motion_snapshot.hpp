#pragma once
#include <array>
#include <cstdint>

namespace switch2 {
// Read-only feature report from the gamepad to the companion sensor driver.
// Transport is shared without a second Bluetooth/USB owner or a mapper process.
#pragma pack(push, 1)
struct MotionSnapshot {
    uint8_t id{0xe2}, version{1}, transport{}, ready{};
    uint32_t sequence{};
    uint64_t updated_ms{};
    std::array<float, 3> angular_velocity{}, acceleration{};
    uint64_t published_qpc{}; // Host monotonic clock for measuring inter-driver delivery latency.
    uint64_t sample_time_100ns{};
    uint8_t reserved[8]{};
};
struct MotionDeliveryStatistics {
    uint8_t id{0xe5}, version{1};
    uint16_t reserved{};
    uint32_t samples{}, sequence_gaps{}, stale{}, gyro_dropped{}, accel_dropped{};
    uint64_t latency_qpc_sum{}, latency_qpc_max{}, qpc_frequency{};
    uint8_t padding[16]{};
};
#pragma pack(pop)
static_assert(sizeof(MotionSnapshot) == 64);
static_assert(sizeof(MotionDeliveryStatistics) == 64);
} // namespace switch2
