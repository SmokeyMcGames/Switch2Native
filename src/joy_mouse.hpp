#pragma once
#include "protocol.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace switch2 {
enum MouseOption : uint8_t { MouseEnabled = 1, MouseGyro = 2, MousePaused = 4 };
struct JoyMouseSample {
    int16_t dx{}, dy{};
    uint8_t surface{255}, buttons{};
    int8_t sensitivity{}; // Home +1, C/Capture -1, released or both held 0.
};
std::optional<JoyMouseSample> decode_joy_mouse(std::span<const uint8_t> bytes);
float mouse_scroll_axis(uint16_t value, const AxisCalibration &calibration);
inline uint8_t joy_mouse_devices(uint8_t connected, uint8_t right_sides) {
    const auto right = uint8_t(connected & right_sides);
    return right ? right : connected;
}

#pragma pack(push, 1)
struct MouseReport {
    uint8_t id{0xed}, buttons{};
    int16_t dx{}, dy{};
    int8_t wheel{};
};
#pragma pack(pop)
static_assert(sizeof(MouseReport) == 7);

// One desktop pointer can be driven by several physical Joy-Cons. Button state
// belongs to each device so disconnecting one cannot release another's drag.
class JoyMouse {
  public:
    std::optional<MouseReport> update(unsigned device, const JoyMouseSample &sample, bool enabled,
                                      float scroll = 0, uint64_t now_ms = 0, bool gyro = false);
    std::optional<MouseReport> release(unsigned device);
    std::optional<MouseReport> move_gyro(unsigned device, double dx, double dy, uint64_t now_ms = 0,
                                         double counts_per_degree = 90);
    void reset_gyro(unsigned device);
    bool active(unsigned device) const { return devices_.at(device).active; }
    uint8_t active_mask() const;
    uint8_t sensitivity_level() const { return sensitivity_level_; }

  private:
    struct Device {
        bool active{}, scroll_blocked{};
        uint8_t buttons{}, blocked{};
        int8_t previous_sensitivity{};
        int remainder_x{}, remainder_y{};
        double gyro_remainder_x{}, gyro_remainder_y{};
        double click_motion_x{}, click_motion_y{};
        uint64_t click_hold_until_ms{};
        int scroll_direction{};
        uint64_t next_scroll_ms{};
    };
    std::array<Device, 4> devices_{};
    uint8_t buttons_{};
    uint8_t sensitivity_level_{4}; // Quarter-speed units, shared by both halves.
    std::optional<MouseReport> report(int16_t dx, int16_t dy, int8_t wheel = 0);
    void adjust_sensitivity(int change);
    int16_t scale_motion(int16_t delta, int &remainder) const;
};
} // namespace switch2
