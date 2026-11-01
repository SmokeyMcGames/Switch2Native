#pragma once
#include "protocol.hpp"
#include <cstdint>

namespace switch2 {
enum class JoyMode : uint8_t { Single, Separate, Combined };
enum class JoyCommand : uint8_t { Choose = 1, Rumble = 2, MouseMode = 3, MouseSource = 4, Profile = 5 };
enum JoyFrameFlag : uint8_t {
    JoyConnected = 1, JoyMotionReady = 2, JoyUsb = 4, JoyMouseEligible = 8,
    JoyGyroMouse = 16, JoyGyroPaused = 32, JoyRight = 64, JoySideways = 128
};
#pragma pack(push, 1)
inline constexpr unsigned MaxJoyCons = 4;
inline constexpr std::array<uint8_t, MaxJoyCons> JoySnapshotIds{0xe8, 0xe9, 0xeb, 0xec};
struct JoyStatus {
    uint8_t id{0xe7}, version{2}, connected{}, active_slots{};
    JoyMode mode{JoyMode::Single};
    uint8_t first_side{}, needs_choice{}, owners{}; // needs_choice is a physical pair mask
    uint32_t generation{};
    uint8_t right_sides{}, groups[MaxJoyCons]{}, pairs[2]{}, reserved{};
    uint32_t errors[MaxJoyCons]{}, reports[MaxJoyCons]{};
    uint8_t mouse_enabled{}, mouse_ready{}, mouse_active{}, mouse_errors{};
    uint8_t mouse_sensitivity{4}, mouse_source{}, gyro_paused{}, padding[5]{}; // Gain in 25% steps; source 0 optical, 1 gyro.
};
struct JoyFrame {
    uint8_t id{0xe6}, version{2}, slot{}, flags{}; // bit 0 connected, bit 1 motion ready
    uint32_t sequence{};
    std::array<uint8_t, 16> controls{}; // Byte15 is reserved by the gamepad; E6 uses it for IMU interval in 1.25ms ticks.
    std::array<float, 3> gyro{}, acceleration{};
    uint64_t updated_ms{};
    uint64_t sample_time_100ns{};
};
struct JoyRequest {
    uint8_t id{0xea}, version{2};
    JoyCommand command{JoyCommand::Choose};
    uint8_t value{}; // mode for Choose, slot for Rumble
    uint32_t generation{};
    uint8_t strong{}, weak{}, pair{}, padding[53]{};
};
// Pro-only diagnostic on the hub's otherwise unused E9 feature report.
struct ProConnectionTrace {
    uint8_t id{0xe9}, version{1}, reserved[2]{};
    uint32_t sequence{};
    uint64_t updated_ms{};
    char message[48]{};
};
#pragma pack(pop)
static_assert(sizeof(ProConnectionTrace) == 64);
static_assert(sizeof(JoyStatus) == 64 && sizeof(JoyFrame) == 64 && sizeof(JoyRequest) == 64);

// Pure connection policy. A choice belongs to one uninterrupted two-controller
// connection, so a late dialog cannot reconfigure a different connection.
class JoyRouting {
  public:
    bool connect(unsigned device, bool connected);
    bool connect(unsigned device, bool connected, bool right);
    bool choose(uint32_t generation, JoyMode mode, uint8_t pair = 0);
    JoyStatus status() const;
    unsigned slot_for(unsigned device) const;
    uint8_t physical_mask(unsigned slot) const;

  private:
    uint8_t connected_{}, right_sides_{0x0a};
    std::array<uint8_t, MaxJoyCons> partners_{4, 4, 4, 4};
    std::array<bool, MaxJoyCons> combined_{}, pending_{};
    std::array<uint64_t, MaxJoyCons> arrival_{};
    uint64_t next_arrival_{};
    uint32_t generation_{};
};

std::array<uint8_t, 16> single_joy_controls(unsigned side, const State &state,
                                            const Calibration &calibration);
std::array<uint8_t, 16> combined_joy_controls(const State &left,
                                              const Calibration &left_calibration,
                                              const State &right,
                                              const Calibration &right_calibration);
Motion sideways_motion(unsigned side, Motion motion);
uint8_t joy_player_lights(bool right, unsigned player, bool sideways);
} // namespace switch2
