#include "protocol.hpp"
#include <algorithm>
#include <cmath>

namespace switch2 {
static void stick(const uint8_t *data, uint16_t &x, uint16_t &y) {
    x = uint16_t(data[0] | ((data[1] & 15) << 8));
    y = uint16_t((data[1] >> 4) | (data[2] << 4));
}
static State decode_common(std::span<const uint8_t> data, size_t buttons, size_t axes) {
    State state;
    for (size_t i = 0; i < 4; ++i)
        state.buttons |= uint32_t(data[buttons + i]) << (8 * i);
    stick(data.data() + axes, state.sticks[0], state.sticks[1]);
    stick(data.data() + axes + 3, state.sticks[2], state.sticks[3]);
    const size_t motion = axes + 32;
    for (size_t i = 0; i < 4; ++i)
        state.raw_motion.timestamp |= uint32_t(data[motion + i]) << (8 * i);
    for (size_t i = 0; i < 3; ++i) {
        const size_t accel = motion + 6 + 2 * i, gyro = motion + 12 + 2 * i;
        state.raw_motion.acceleration[i] = int16_t(data[accel] | (uint16_t(data[accel + 1]) << 8));
        state.raw_motion.angular_velocity[i] =
            int16_t(data[gyro] | (uint16_t(data[gyro + 1]) << 8));
    }
    return state;
}
std::optional<State> decode_usb(std::span<const uint8_t> data) {
    if (data.size() != 64)
        return std::nullopt;
    if (data.size() >= 17 && data[0] == 0x05)
        return decode_common(data, 5, 11);
    if (data.size() < 12 || data[0] != 0x09)
        return std::nullopt;
    State state;
    constexpr uint32_t right[] = {B, A, Y, X, R, ZR, Plus, R3};
    constexpr uint32_t left[] = {Down, Right, Left, Up, L, ZL, Minus, L3};
    constexpr uint32_t extra[] = {Home, Capture, GR, GL, Chat};
    for (unsigned i = 0; i < 8; ++i) {
        if (data[3] & (1u << i))
            state.buttons |= right[i];
        if (data[4] & (1u << i))
            state.buttons |= left[i];
    }
    for (unsigned i = 0; i < 5; ++i)
        if (data[5] & (1u << i))
            state.buttons |= extra[i];
    stick(data.data() + 6, state.sticks[0], state.sticks[1]);
    stick(data.data() + 9, state.sticks[2], state.sticks[3]);
    return state;
}
std::optional<State> decode_ble(std::span<const uint8_t> data) {
    if (data.size() != 63)
        return std::nullopt;
    return decode_common(data, 4, 10);
}
std::optional<State> decode_packed_joy_controls(std::span<const uint8_t> data) {
    if (data.size() != 64 || (data[0] != 7 && data[0] != 8)) return std::nullopt;
    State state;
    const bool right = data[0] == 8;
    constexpr uint32_t right_buttons[]{B, A, Y, X, R, ZR, Plus, R3};
    constexpr uint32_t left_buttons[]{Down, Right, Left, Up, L, ZL, Minus, L3};
    for (unsigned bit = 0; bit < 8; ++bit)
        if (data[3] & (1u << bit)) state.buttons |= right ? right_buttons[bit] : left_buttons[bit];
    if (data[4] & 1) state.buttons |= right ? Home : Capture;
    if (right && (data[4] & 0x10)) state.buttons |= Chat;
    if (data[4] & 0x40) state.buttons |= right ? SRRight : SRLeft;
    if (data[4] & 0x80) state.buttons |= right ? SLRight : SLLeft;
    const unsigned axis = right ? 2 : 0;
    stick(data.data() + 6, state.sticks[axis], state.sticks[axis + 1]);
    return state;
}
bool parse_calibration(std::span<const uint8_t> data, Calibration &calibration, size_t axis) {
    if (data.size() < 9 || axis > 2)
        return false;
    uint16_t centers[2], positive[2], negative[2];
    stick(data.data(), centers[0], centers[1]);
    stick(data.data() + 3, positive[0], positive[1]);
    stick(data.data() + 6, negative[0], negative[1]);
    for (size_t i = 0; i < 2; ++i) {
        if (centers[i] < 512 || centers[i] > 3583 || negative[i] < 256 || positive[i] < 256 ||
            negative[i] > 4095 || positive[i] > 4095)
            return false;
    }
    for (size_t i = 0; i < 2; ++i)
        calibration[axis + i] = {centers[i], negative[i], positive[i]};
    return true;
}
std::array<float, 4> stick_positions(const State &state, const Calibration &calibration, bool filtered) {
    std::array<float, 4> result;
    for (size_t i = 0; i < result.size(); ++i) {
        const auto delta = int(state.sticks[i]) - int(calibration[i].center);
        result[i] = std::clamp(float(delta) / float(std::max(1, int(delta < 0 ?
            calibration[i].negative : calibration[i].positive))), -1.0f, 1.0f);
    }
    if (!filtered) return result;
    for (unsigned stick = 0; stick < 2; ++stick) {
        auto &x = result[stick * 2], &y = result[stick * 2 + 1];
        const float deadzone = std::min<unsigned>(state.profile.deadzone[stick], 40) / 100.0f;
        if (state.profile.circular_mask & (1u << stick)) {
            const float radius = std::hypot(x, y);
            const float scale = radius <= deadzone ? 0 :
                (std::min(radius, 1.0f) - deadzone) / ((1.0f - deadzone) * radius);
            x *= scale;
            y *= scale;
        } else {
            for (auto *axis : {&x, &y})
                *axis = std::abs(*axis) <= deadzone ? 0.0f :
                    std::copysign((std::abs(*axis) - deadzone) / (1.0f - deadzone), *axis);
        }
    }
    return result;
}
std::array<uint8_t, 16> xbox_controls(const State &state, const Calibration &calibration) {
    std::array<uint8_t, 16> result{};
    auto put = [&](size_t index, uint16_t value) {
        result[index] = uint8_t(value);
        result[index + 1] = uint8_t(value >> 8);
    };
    const auto sticks = stick_positions(state, calibration, true);
    for (size_t i = 0; i < 4; ++i) {
        float value = sticks[i];
        if (i % 2)
            value = -value;
        put(2 * i,
            uint16_t(std::clamp(32768 + int(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767)), 0,
                                65534)));
    }
    const auto buttons = state.buttons;
    put(8, buttons & ZL ? 1023 : 0);
    put(10, buttons & ZR ? 1023 : 0);
    const int horizontal = bool(buttons & Right) - bool(buttons & Left);
    const int vertical = bool(buttons & Down) - bool(buttons & Up);
    constexpr uint8_t hats[3][3] = {{8, 1, 2}, {7, 0, 3}, {6, 5, 4}};
    result[12] = hats[vertical + 1][horizontal + 1];
    // Match physical positions: Nintendo B is Xbox A, Nintendo A is Xbox B.
    const uint32_t mapped[] = {B, A, Y, X, L, R, Minus, Plus};
    for (unsigned i = 0; i < 8; ++i)
        if (buttons & mapped[i])
            result[13] |= uint8_t(1u << i);
    // Home is an ordinary HID button, never a System Main Menu/Guide command.
    result[14] = uint8_t((buttons & L3 ? 1u : 0u) | (buttons & R3 ? 2u : 0u) | (buttons & Home ? 4u : 0u));
    return result;
}
std::array<uint8_t, 64> usb_rumble(uint8_t sequence, uint8_t strong, uint8_t weak) {
    // SDL's Switch 2 encoding, with a lower amplitude ceiling than SDL's 29000.
    const uint16_t high_frequency = 0x187, low_frequency = 0x112;
    const uint16_t low = uint16_t(uint32_t(strong) * 16000 / 255),
                   high = uint16_t(uint32_t(weak) * 16000 / 255);
    std::array<uint8_t, 64> report{};
    report[0] = 2;
    report[1] = uint8_t(0x50 | (sequence & 15));
    report[2] = uint8_t(high_frequency & 0xff);
    report[3] = uint8_t(((high >> 4) & 0xfc) | ((high_frequency >> 8) & 3));
    report[4] = uint8_t((high >> 12) | (low_frequency << 4));
    report[5] = uint8_t((low & 0xc0) | ((low_frequency >> 4) & 0x3f));
    report[6] = uint8_t(low >> 8);
    std::copy_n(report.begin() + 1, 6, report.begin() + 17);
    return report;
}
std::array<uint8_t, 33> ble_rumble(uint8_t sequence, uint8_t strong, uint8_t weak) {
    auto usb = usb_rumble(sequence, strong, weak);
    std::array<uint8_t, 33> report{};
    std::copy_n(usb.begin() + 1, 32, report.begin() + 1);
    return report;
}
} // namespace switch2
