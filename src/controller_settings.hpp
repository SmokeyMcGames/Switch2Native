#pragma once
#include "motion_orientation.hpp"
#include "joy_protocol.hpp"
#include "button_preview.hpp"
#include <mutex>
#include <string>

namespace switch2 {
#pragma pack(push, 1)
struct ControllerSnapshot {
    uint8_t id{0xf0}, version{1};
    ControllerKind kind{ControllerKind::Pro};
    uint8_t connected{}, player{}, sideways{}, identity_source{};
    std::array<char, 16> serial{};
    std::array<int16_t, 4> raw_sticks{}, output_sticks{};
    ControllerProfile profile;
    uint8_t transport{}, diagnostic_flags{}; // bit 0: physical button telemetry available
    uint32_t buttons{}, recent_buttons{}, input_tick_ms{};
    uint8_t reserved[2]{};
};
struct ControllerMotion {
    uint8_t id{0xf4}, version{1}, ready{}, reserved{};
    uint32_t samples{};
    uint64_t sample_time_100ns{};
    std::array<float, 3> gyro{}, acceleration{};  // degrees/s and g
    std::array<float, 4> orientation{1, 0, 0, 0}; // w, x, y, z; body to world
    uint8_t padding[8]{};
};
#pragma pack(pop)
static_assert(sizeof(ControllerSnapshot) == 64 && sizeof(ControllerMotion) == 64);
ControllerSnapshot controller_identity(ControllerKind kind, std::span<const uint8_t> factory,
                                       uint64_t bluetooth_address);
std::wstring controller_profile_key(const ControllerSnapshot &controller);
ControllerProfile load_controller_profile(const ControllerSnapshot &controller);
bool save_controller_profile(const ControllerSnapshot &controller,
                             const ControllerProfile &profile);
JoyRequest profile_request(unsigned device, const ControllerSnapshot &controller,
                           const ControllerProfile &profile);

class ControllerSettingsState {
  public:
    void connect(ControllerKind kind, std::span<const uint8_t> factory, uint64_t bluetooth_address);
    void disconnect();
    ControllerProfile profile() const;
    ControllerSnapshot snapshot() const;
    ControllerMotion motion() const;
    bool apply(const std::array<char, 16> &serial, uint8_t identity_source,
               const ControllerProfile &profile);
    bool apply(const JoyRequest &request);
    void update(const State &raw, const State &output, const Calibration &calibration,
                unsigned player, bool sideways, uint8_t transport);
    void update_motion(const Motion &motion);

  private:
    mutable std::mutex mutex_;
    ControllerSnapshot state_;
    ControllerMotion motion_;
    MotionOrientation orientation_;
    ButtonPreview buttons_;
};
} // namespace switch2
