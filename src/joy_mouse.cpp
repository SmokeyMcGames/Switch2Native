#include "joy_mouse.hpp"
#include "gyro_motion.hpp"
#include <algorithm>
#include <cmath>

namespace switch2 {
std::optional<JoyMouseSample> decode_joy_mouse(std::span<const uint8_t> bytes) {
    if (bytes.size() != 64 || (bytes[0] != 7 && bytes[0] != 8)) return {};
    const auto word = [&](size_t offset) {
        return int16_t(uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8));
    };
    const auto buttons = uint8_t(((bytes[3] >> 4) & 3) | ((bytes[3] & 0x08) ? 4 : 0));
    const bool right = bytes[0] == 8;
    const bool increase = right && (bytes[4] & 1);
    const bool decrease = right ? (bytes[4] & 0x10) != 0 : (bytes[4] & 1) != 0;
    const auto sensitivity = int8_t(int(increase) - int(decrease));
    return JoyMouseSample{word(10), word(12), bytes[14], buttons, sensitivity};
}
float mouse_scroll_axis(uint16_t value, const AxisCalibration &calibration) {
    const int delta = int(value) - int(calibration.center);
    const int extent = delta < 0 ? calibration.negative : calibration.positive;
    return std::clamp(float(delta) / float(std::max(1, extent)), -1.0f, 1.0f);
}
std::optional<MouseReport> JoyMouse::report(int16_t dx, int16_t dy, int8_t wheel) {
    uint8_t buttons = 0;
    for (const auto &device : devices_) buttons |= device.buttons;
    if (!dx && !dy && !wheel && buttons == buttons_) return {};
    buttons_ = buttons;
    return MouseReport{0xed, buttons, dx, dy, wheel};
}
std::optional<MouseReport> JoyMouse::release(unsigned device) {
    devices_.at(device) = {};
    return report(0, 0);
}
void JoyMouse::reset_gyro(unsigned device) {
    auto &state = devices_.at(device);
    state.gyro_remainder_x = state.gyro_remainder_y = 0;
    state.click_motion_x = state.click_motion_y = 0;
    state.click_hold_until_ms = 0;
}
std::optional<MouseReport> JoyMouse::move_gyro(unsigned device, double dx, double dy, uint64_t now_ms,
                                              double counts_per_degree) {
    auto &state = devices_.at(device);
    if (!state.active || !std::isfinite(dx) || !std::isfinite(dy) ||
        !std::isfinite(counts_per_degree) || counts_per_degree < 10 || counts_per_degree > 360) return {};
    if (state.click_hold_until_ms) {
        state.click_motion_x += dx;
        state.click_motion_y += dy;
        // Net travel rejects back-and-forth click tremor. A deliberate 0.4-degree
        // drag can break the hold early, but only while a click is still held.
        const double drag_distance = 0.4 * counts_per_degree;
        const bool drag = (state.buttons & 3) &&
            std::hypot(state.click_motion_x, state.click_motion_y) >= drag_distance;
        if (now_ms < state.click_hold_until_ms && !drag) return {};
        reset_gyro(device);
        // Discard the suppressed path; only this fresh sample may move the
        // pointer. Replaying the backlog would jump away from the click target.
    }
    const auto quantize = [](double delta, double &remainder) {
        const double value = std::clamp(delta + remainder, -32768.0, 32767.0);
        const auto counts = int16_t(value);
        remainder = value - counts;
        return counts;
    };
    // Gyro response is independent of the optical sensitivity setting.
    // Only fractional counts are retained; there is no screen-position target.
    return report(quantize(dx, state.gyro_remainder_x), quantize(dy, state.gyro_remainder_y));
}
void JoyMouse::adjust_sensitivity(int change) {
    const auto level = uint8_t(std::clamp(int(sensitivity_level_) + change, 1, 16));
    if (level == sensitivity_level_) return;
    sensitivity_level_ = level;
    for (auto &device : devices_) {
        device.remainder_x = device.remainder_y = 0;
    }
}
int16_t JoyMouse::scale_motion(int16_t delta, int &remainder) const {
    // Keep sub-count movement at low sensitivity without mixing two sensors'
    // fractional positions, and never wrap a large signed HID delta.
    const int scaled = int(delta) * sensitivity_level_ + remainder;
    remainder = scaled % 4;
    return int16_t(std::clamp(scaled / 4, -32768, 32767));
}
std::optional<MouseReport> JoyMouse::update(unsigned index, const JoyMouseSample &sample, bool enabled,
                                           float scroll, uint64_t now_ms, bool gyro) {
    auto &device = devices_.at(index);
    // Hardware capture: contact measured 28-84, lifted/sideways resting near
    // 250-255; zero occurred only before the sensor started. Hysteresis avoids
    // alternating gamepad/mouse buttons at the edge of the usable surface.
    const unsigned limit = device.active ? 160 : 128;
    if (!enabled || !sample.surface || sample.surface > limit) return release(index);
    // Placing a controller down while already holding a bumper must not click.
    constexpr float deadzone = 0.25f;
    const float amount = std::isfinite(scroll) ? std::clamp(std::abs(scroll), 0.0f, 1.0f) : 0.0f;
    if (!device.active) {
        device.blocked = sample.buttons;
        device.scroll_blocked = amount > deadzone;
        device.previous_sensitivity = sample.sensitivity;
    }
    device.active = true;
    device.blocked &= sample.buttons;
    const auto previous_buttons = device.buttons;
    device.buttons = uint8_t(sample.buttons & ~device.blocked);
    if (gyro && (device.buttons & ~previous_buttons & 3)) {
        reset_gyro(index);
        device.click_hold_until_ms = now_ms + 200;
    } else if (!gyro) {
        reset_gyro(index);
    }
    if (sample.sensitivity && !device.previous_sensitivity)
        adjust_sensitivity(sample.sensitivity);
    device.previous_sensitivity = sample.sensitivity;
    int8_t wheel = 0;
    if (amount <= deadzone) {
        device.scroll_blocked = false;
        device.scroll_direction = 0;
    } else if (!device.scroll_blocked) {
        const int direction = scroll > 0 ? 1 : -1;
        if (direction != device.scroll_direction || now_ms >= device.next_scroll_ms) {
            wheel = int8_t(direction);
            const float steps_per_second = 2.0f + 10.0f * (amount - deadzone) / (1.0f - deadzone);
            device.next_scroll_ms = now_ms + uint64_t(std::lround(1000.0f / steps_per_second));
            device.scroll_direction = direction;
        }
    }
    // Never catch up missed scroll ticks after a stalled Bluetooth packet.
    return report(scale_motion(sample.dx, device.remainder_x),
                  scale_motion(sample.dy, device.remainder_y), wheel);
}
uint8_t JoyMouse::active_mask() const {
    uint8_t mask = 0;
    for (unsigned index = 0; index < devices_.size(); ++index)
        if (devices_[index].active) mask |= uint8_t(1u << index);
    return mask;
}
} // namespace switch2
