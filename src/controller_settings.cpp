#include "controller_settings.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace switch2 {
ControllerSnapshot controller_identity(ControllerKind kind, std::span<const uint8_t> factory,
                                       uint64_t address) {
    ControllerSnapshot result;
    result.kind = kind;
    if (factory.size() >= 22 && factory[18] == 0x7e && factory[19] == 5 &&
        (unsigned(factory[20]) | (unsigned(factory[21]) << 8)) == unsigned(kind)) {
        std::copy_n(factory.begin() + 2, 16, result.serial.begin());
        result.identity_source = 1;
        if (!controller_profile_key(result).empty())
            return result;
    }
    result.serial = {};
    result.identity_source = 0;
    if (address && address < (1ull << 48)) {
        std::snprintf(result.serial.data(), result.serial.size(), "%012llX", address);
        result.identity_source = 2;
    }
    return result;
}
std::wstring controller_profile_key(const ControllerSnapshot &controller) {
    if (controller.kind != ControllerKind::Pro && controller.kind != ControllerKind::JoyLeft &&
        controller.kind != ControllerKind::JoyRight)
        return {};
    if (controller.identity_source != 1 && controller.identity_source != 2)
        return {};
    std::wstring serial;
    bool ended = false;
    for (char value : controller.serial) {
        if (!value) {
            ended = true;
            continue;
        }
        if (ended || !((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')))
            return {};
        serial += wchar_t(value);
    }
    if (serial.empty() || (controller.identity_source == 2 && serial.size() != 12))
        return {};
    return L"SOFTWARE\\Switch2Native\\Controllers\\" + std::to_wstring(unsigned(controller.kind)) +
           L"-" + std::to_wstring(controller.identity_source) + L"-" + serial;
}
ControllerProfile load_controller_profile(const ControllerSnapshot &controller) {
    const auto key = controller_profile_key(controller);
    ControllerProfile profile;
    DWORD bytes = sizeof(profile);
    if (key.empty() ||
        RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Profile", RRF_RT_REG_BINARY, nullptr,
                     &profile, &bytes) != ERROR_SUCCESS ||
        bytes != sizeof(profile) || !valid_profile(profile))
        return {};
    return profile;
}
bool save_controller_profile(const ControllerSnapshot &controller,
                             const ControllerProfile &profile) {
    const auto path = controller_profile_key(controller);
    if (path.empty() || !valid_profile(profile))
        return false;
    HKEY key{};
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    // One binary value commits the entire validated profile atomically.
    const auto result = RegSetValueExW(key, L"Profile", 0, REG_BINARY,
                                       reinterpret_cast<const BYTE *>(&profile), sizeof(profile));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
void ControllerSettingsState::connect(ControllerKind kind, std::span<const uint8_t> factory,
                                      uint64_t address) {
    auto identity = controller_identity(kind, factory, address);
    identity.profile = load_controller_profile(identity);
    std::lock_guard lock(mutex_);
    state_ = identity;
    buttons_ = {};
    motion_ = {};
    orientation_ = {};
}
void ControllerSettingsState::disconnect() {
    std::lock_guard lock(mutex_);
    state_.connected = 0;
    state_.buttons = state_.recent_buttons = state_.input_tick_ms = 0;
    state_.diagnostic_flags = 0;
    buttons_ = {};
    state_.raw_sticks = state_.output_sticks = {};
    motion_ = {};
    orientation_ = {};
}
ControllerProfile ControllerSettingsState::profile() const {
    std::lock_guard lock(mutex_);
    return state_.profile;
}
JoyRequest profile_request(unsigned device, const ControllerSnapshot &controller,
                           const ControllerProfile &profile) {
    JoyRequest request;
    request.command = JoyCommand::Profile;
    request.value = uint8_t(device);
    std::memcpy(request.padding, controller.serial.data(), 16);
    request.padding[16] = controller.identity_source;
    std::memcpy(request.padding + 17, &profile, sizeof(profile));
    return request;
}
bool ControllerSettingsState::apply(const JoyRequest &request) {
    if (request.version != 2 || request.command != JoyCommand::Profile)
        return false;
    std::array<char, 16> serial;
    ControllerProfile profile;
    std::memcpy(serial.data(), request.padding, 16);
    std::memcpy(&profile, request.padding + 17, sizeof(profile));
    return apply(serial, request.padding[16], profile);
}
ControllerSnapshot ControllerSettingsState::snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = state_;
    result.recent_buttons = buttons_.recent(uint32_t(GetTickCount64()));
    return result;
}
ControllerMotion ControllerSettingsState::motion() const {
    std::lock_guard lock(mutex_);
    return motion_;
}
bool ControllerSettingsState::apply(const std::array<char, 16> &serial, uint8_t source,
                                    const ControllerProfile &profile) {
    if (!valid_profile(profile))
        return false;
    std::lock_guard lock(mutex_);
    if (!state_.connected || source != state_.identity_source || serial != state_.serial ||
        controller_profile_key(state_).empty())
        return false;
    state_.profile = profile;
    return true;
}
void ControllerSettingsState::update(const State &raw, const State &output,
                                     const Calibration &calibration, unsigned player, bool sideways,
                                     uint8_t transport) {
    auto input = stick_positions(raw, calibration, false);
    auto filtered = stick_positions(output, calibration, true);
    std::lock_guard lock(mutex_);
    state_.connected = 1;
    state_.player = uint8_t(player);
    state_.sideways = sideways;
    state_.transport = transport;
    state_.buttons = raw.buttons;
    state_.input_tick_ms = uint32_t(GetTickCount64());
    state_.diagnostic_flags = 1;
    buttons_.update(raw.buttons, state_.input_tick_ms);
    // Match the single sideways gamepad's orientation without hiding drift in
    // the raw dot. Paired halves and the Pro retain their upright axes.
    if (sideways) {
        const bool right = state_.kind == ControllerKind::JoyRight;
        const unsigned axis = right ? 2 : 0;
        for (auto *values : {&input, &filtered}) {
            const float x = (*values)[axis], y = (*values)[axis + 1];
            (*values)[axis] = right ? y : -y;
            (*values)[axis + 1] = right ? -x : x;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        state_.raw_sticks[i] = int16_t(std::lround(input[i] * 32767));
        state_.output_sticks[i] = int16_t(std::lround(filtered[i] * 32767));
    }
}
void ControllerSettingsState::update_motion(const Motion &sample) {
    if (!sample.ready)
        return;
    std::lock_guard lock(mutex_);
    if (sample.sample_time_100ns <= motion_.sample_time_100ns)
        return;
    orientation_.update(sample);
    motion_.ready = 1;
    ++motion_.samples;
    motion_.sample_time_100ns = sample.sample_time_100ns;
    motion_.gyro = sample.angular_velocity;
    for (unsigned i = 0; i < 3; ++i)
        motion_.acceleration[i] = sample.acceleration[i] / 9.80665f;
    motion_.orientation = orientation_.quaternion();
}
} // namespace switch2
