#pragma once
#include "joy_protocol.hpp"
#include "joy_mouse.hpp"
#include "mouse_shortcuts.hpp"
#include "gyro_motion.hpp"
#include "controller_settings.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace switch2 {
class JoySession {
  public:
    using Publish = std::function<void(const JoyFrame &)>;
    using PublishMouse = std::function<void(const MouseReport &, bool clear_pending)>;
    JoySession(Publish publish, PublishMouse publish_mouse);
    ~JoySession();
    JoyStatus status() const;
    JoyFrame latest(unsigned slot) const;
    bool request(const JoyRequest &request);
    ControllerSettingsState &settings(unsigned device) { return settings_.at(device); }

  private:
    void run(unsigned side, std::stop_token stop);
    void update(unsigned side, const State &state, const Calibration &calibration, bool connected,
                bool right = false, bool physical_report = true);
    struct Configuration {
        uint64_t address{};
        bool right{};
    };
    static Configuration configured_device(unsigned device);
    Publish publish_;
    PublishMouse publish_mouse_;
    std::mutex mouse_mutex_;
    JoyMouse mouse_;
    std::atomic<uint8_t> mouse_options_{};
    std::array<MouseShortcuts, MaxJoyCons> mouse_shortcuts_{};
    std::array<uint8_t, MaxJoyCons> mouse_previous_options_{};
    std::array<bool, MaxJoyCons> mouse_previous_stick_{};
    std::array<GyroMotion, MaxJoyCons> gyro_motion_{};
    std::array<bool, MaxJoyCons> gyro_moving_{};
    std::atomic<uint8_t> mouse_allowed_{};
    std::atomic<uint8_t> mouse_ready_{}, mouse_active_{}, mouse_errors_{};
    std::atomic<uint8_t> mouse_sensitivity_{4};
    void process_mouse(unsigned device, bool right, State &state, const Calibration &calibration,
                       std::span<const uint8_t> bytes, bool optical_ready, bool gyro_ready);
    void release_mouse(unsigned device, bool reset_shortcuts = false);
    void process_gyro(unsigned device, const Motion &motion);
    uint8_t change_mouse_options(uint8_t clear, uint8_t set, uint8_t toggle = 0);
    mutable std::mutex mutex_;
    JoyRouting routing_;
    std::array<State, MaxJoyCons> states_{};
    std::array<ControllerSettingsState, MaxJoyCons> settings_;
    std::array<State, MaxJoyCons> raw_states_{};
    std::array<Calibration, MaxJoyCons> calibration_{};
    std::array<JoyFrame, MaxJoyCons> frames_{};
    std::array<uint32_t, MaxJoyCons> reports_{}, errors_{};
    std::array<std::atomic<uint16_t>, MaxJoyCons> rumble_{};
    std::atomic<bool> refresh_{};
    std::atomic<uint8_t> owners_{};
    uint32_t sequence_{};
    std::array<std::jthread, MaxJoyCons> threads_;
};
} // namespace switch2
