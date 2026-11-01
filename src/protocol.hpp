#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include "controller_profile.hpp"

namespace switch2 {
enum class ControllerKind : uint16_t { JoyLeft = 0x2067, JoyRight = 0x2066, Pro = 0x2069 };
enum Button : uint32_t {
    Y = 1u << 0,
    X = 1u << 1,
    B = 1u << 2,
    A = 1u << 3,
    SRRight = 1u << 4,
    SLRight = 1u << 5,
    R = 1u << 6,
    ZR = 1u << 7,
    Minus = 1u << 8,
    Plus = 1u << 9,
    R3 = 1u << 10,
    L3 = 1u << 11,
    Home = 1u << 12,
    Capture = 1u << 13,
    Chat = 1u << 14,
    Down = 1u << 16,
    Up = 1u << 17,
    Right = 1u << 18,
    Left = 1u << 19,
    SRLeft = 1u << 20,
    SLLeft = 1u << 21,
    L = 1u << 22,
    ZL = 1u << 23,
    GR = 1u << 24,
    GL = 1u << 25
};
struct RawMotion {
    uint32_t timestamp{};
    std::array<int16_t, 3> acceleration{}, angular_velocity{};
};
struct Motion {
    bool ready{};
    std::array<float, 3> acceleration{}, angular_velocity{}; // m/s^2 and degrees/s
    uint64_t sample_time_100ns{}; // Device sample time mapped to Windows FILETIME.
    uint32_t sample_interval_100ns{}; // Original device interval, without host-clock slew.
};
struct State {
    uint32_t buttons{};
    std::array<uint16_t, 4> sticks{2048, 2048, 2048, 2048};
    RawMotion raw_motion;
    Motion motion;
    ControllerProfile profile;
};
struct AxisCalibration {
    uint16_t center{2048}, negative{1800}, positive{1800};
};
using Calibration = std::array<AxisCalibration, 4>;
std::array<float, 4> stick_positions(const State &state, const Calibration &calibration, bool filtered);
bool parse_calibration(std::span<const uint8_t> data, Calibration &calibration, size_t axis);
std::array<uint8_t, 16> xbox_controls(const State &state, const Calibration &calibration);
std::array<uint8_t, 64> usb_rumble(uint8_t sequence, uint8_t strong, uint8_t weak);
std::array<uint8_t, 33> ble_rumble(uint8_t sequence, uint8_t strong, uint8_t weak);
std::optional<State> decode_usb(std::span<const uint8_t> data);
std::optional<State> decode_ble(std::span<const uint8_t> data);
std::optional<State> decode_packed_joy_controls(std::span<const uint8_t> data);
} // namespace switch2
