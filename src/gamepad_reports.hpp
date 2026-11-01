#pragma once
#include <array>
#include <cstdint>

namespace switch2 {
// The Xbox filter interprets the eleventh gamepad bit as Guide even without a
// System Main Menu usage. Keep Home exclusively in the separate HID collection.
inline std::array<uint8_t, 16> gamepad_report(const std::array<uint8_t, 16> &controls) {
    std::array<uint8_t, 16> report{1};
    for (unsigned i = 0; i < 15; ++i) report[i + 1] = controls[i];
    report[15] &= 3;
    return report;
}
inline std::array<uint8_t, 2> home_button_report(const std::array<uint8_t, 16> &controls) {
    return {5, uint8_t((controls[14] & 4) != 0)};
}
} // namespace switch2
