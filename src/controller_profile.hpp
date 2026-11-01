#pragma once
#include <cstdint>

namespace switch2 {
struct ControllerProfile {
    uint8_t version{1}, deadzone[2]{8, 8}, circular_mask{3};
    uint16_t gyro_counts_per_degree{90}, reserved{};
    bool operator==(const ControllerProfile &) const = default;
};
static_assert(sizeof(ControllerProfile) == 8);
inline bool valid_profile(const ControllerProfile &profile) {
    return profile.version == 1 && profile.deadzone[0] <= 40 && profile.deadzone[1] <= 40 &&
           profile.circular_mask <= 3 && profile.gyro_counts_per_degree >= 10 &&
           profile.gyro_counts_per_degree <= 360 && !profile.reserved;
}
} // namespace switch2
