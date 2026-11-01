#include "joy_session.hpp"
#include "ble.hpp"
#include "motion.hpp"
#include "packed_motion_processor.hpp"
#include <winrt/base.h>
#include <bit>

namespace switch2 {
JoySession::JoySession(Publish publish, PublishMouse publish_mouse)
    : publish_(std::move(publish)), publish_mouse_(std::move(publish_mouse)) {
    for (unsigned side = 0; side < MaxJoyCons; ++side) {
        frames_[side].slot = uint8_t(side);
        frames_[side].controls = xbox_controls(State{}, Calibration{});
    }
    for (unsigned side = 0; side < MaxJoyCons; ++side) {
        threads_[side] = std::jthread([this, side](std::stop_token stop) { run(side, stop); });
    }
}
JoySession::~JoySession() {
    for (auto &thread : threads_)
        thread.request_stop();
    for (auto &thread : threads_)
        thread.join();
}
JoySession::Configuration JoySession::configured_device(unsigned side) {
    DWORD pairing_mask = 0, mask_bytes = sizeof(pairing_mask);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", L"JoyPairingMask",
                 RRF_RT_REG_DWORD, nullptr, &pairing_mask, &mask_bytes);
    if (pairing_mask & (1u << side))
        return {};
    Configuration config;
    DWORD kind = 0, kind_bytes = sizeof(kind);
    const auto suffix = std::to_wstring(side + 1);
    const auto address_name = L"JoyAddress" + suffix;
    const auto kind_name = L"JoyKind" + suffix;
    uint64_t address = 0;
    DWORD bytes = sizeof(address);
    const auto found =
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", address_name.c_str(),
                     RRF_RT_REG_QWORD, nullptr, &address, &bytes);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", kind_name.c_str(),
                 RRF_RT_REG_DWORD, nullptr, &kind, &kind_bytes);
    if (found != ERROR_SUCCESS && side < 2) {
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native",
                     side ? L"JoyRightAddress" : L"JoyLeftAddress", RRF_RT_REG_QWORD, nullptr,
                     &address, &bytes);
        kind = uint16_t(side ? ControllerKind::JoyRight : ControllerKind::JoyLeft);
    }
    if (address < (uint64_t{1} << 48) &&
        (kind == uint16_t(ControllerKind::JoyLeft) || kind == uint16_t(ControllerKind::JoyRight))) {
        config.address = address;
        config.right = kind == uint16_t(ControllerKind::JoyRight);
    }
    return config;
}
JoyStatus JoySession::status() const {
    std::lock_guard lock(mutex_);
    auto result = routing_.status();
    result.owners = owners_.load();
    const auto options = mouse_options_.load();
    result.mouse_enabled = (options & MouseEnabled) != 0;
    result.mouse_source = (options & MouseGyro) != 0;
    result.gyro_paused = (options & MousePaused) != 0;
    result.mouse_ready = mouse_ready_.load();
    result.mouse_active = mouse_active_.load();
    result.mouse_errors = mouse_errors_.load();
    result.mouse_sensitivity = mouse_sensitivity_.load();
    for (unsigned side = 0; side < MaxJoyCons; ++side) {
        result.reports[side] = reports_[side];
        result.errors[side] = errors_[side];
    }
    return result;
}
JoyFrame JoySession::latest(unsigned slot) const {
    std::lock_guard lock(mutex_);
    return frames_.at(slot);
}
bool JoySession::request(const JoyRequest &request) {
    if (request.version != 2)
        return false;
    if (request.command == JoyCommand::Profile)
        return request.value < MaxJoyCons && settings_[request.value].apply(request);
    std::lock_guard lock(mutex_);
    if (request.command == JoyCommand::MouseMode || request.command == JoyCommand::MouseSource) {
        if (request.value > 1 || (request.value && !routing_.status().connected)) return false;
        const auto flag = request.command == JoyCommand::MouseMode ? MouseEnabled : MouseGyro;
        change_mouse_options(uint8_t(flag | MousePaused), request.value ? flag : 0);
        return true;
    }
    if (request.command == JoyCommand::Choose) {
        if (!routing_.choose(request.generation, JoyMode(request.value), request.pair))
            return false;
        for (auto &motor : rumble_)
            motor.store(0);
        refresh_.store(true);
        return true;
    }
    if (request.command != JoyCommand::Rumble || request.value >= MaxJoyCons)
        return false;
    const auto mask = routing_.physical_mask(request.value);
    for (unsigned side = 0; side < MaxJoyCons; ++side)
        if (mask & (1u << side))
            rumble_[side].store(uint16_t(request.strong | (unsigned(request.weak) << 8)));
    return true;
}
uint8_t JoySession::change_mouse_options(uint8_t clear, uint8_t set, uint8_t toggle) {
    auto previous = mouse_options_.load();
    uint8_t next;
    do { next = uint8_t(((previous & ~clear) | set) ^ toggle); }
    while (!mouse_options_.compare_exchange_weak(previous, next));
    return next;
}
void JoySession::release_mouse(unsigned device, bool reset_shortcuts) {
    std::lock_guard lock(mouse_mutex_);
    gyro_motion_[device] = {};
    gyro_moving_[device] = false;
    const bool was_active = mouse_.active(device);
    const auto report = mouse_.release(device);
    const bool last_mouse = was_active && !mouse_.active_mask();
    if (report || last_mouse) publish_mouse_(report.value_or(MouseReport{}), last_mouse);
    mouse_active_.store(mouse_.active_mask());
    if (reset_shortcuts) {
        mouse_shortcuts_[device].reset();
        mouse_previous_stick_[device] = false;
        mouse_previous_options_[device] = 0;
    }
}
void JoySession::process_gyro(unsigned device, const Motion &motion) {
    settings_[device].update_motion(motion);
    std::lock_guard lock(mouse_mutex_);
    auto &tracker = gyro_motion_[device];
    auto &was_moving = gyro_moving_[device];
    const auto options = mouse_options_.load();
    const bool allowed = (mouse_allowed_.load() & (1u << device)) != 0;
    const bool moving = allowed && mouse_.active(device) &&
        (options & (MouseEnabled | MouseGyro | MousePaused)) == (MouseEnabled | MouseGyro);
    if (!moving) {
        if (was_moving) tracker.reset();
        was_moving = false;
        mouse_.reset_gyro(device);
        if (allowed) tracker.calibrate(motion);
        else tracker = {};
        return;
    }
    if (!motion.ready) return;
    if (!was_moving || tracker.needs_start()) {
        tracker.begin(motion);
        mouse_.reset_gyro(device);
        was_moving = true;
        return;
    }
    // Use each original physical sample before any sideways gamepad transform.
    // Relative HID reports combine naturally with other mice on every display.
    if (const auto delta = tracker.update(motion)) {
        const double gain = settings_[device].profile().gyro_counts_per_degree;
        const double scale = gain / GyroCountsPerDegree;
        if (const auto report = mouse_.move_gyro(device, delta->x * scale, delta->y * scale,
                                                GetTickCount64(), gain)) publish_mouse_(*report, false);
    }
}
void JoySession::process_mouse(unsigned device, bool right, State &state, const Calibration &calibration,
                               std::span<const uint8_t> bytes, bool optical_ready, bool gyro_ready) {
    // Serialize aggregation and publication together, without holding mutex_.
    // HID status requests acquire the device lock before mutex_.
    std::lock_guard lock(mouse_mutex_);
    const auto bit = uint8_t(1u << device);
    const bool allowed = (mouse_allowed_.load() & bit) != 0;
    const auto publish = [&](const std::optional<MouseReport> &report) { if (report) publish_mouse_(*report, false); };
    if (!allowed) {
        mouse_ready_.fetch_and(uint8_t(~bit));
        mouse_shortcuts_[device].reset();
        mouse_previous_stick_[device] = false;
        mouse_previous_options_[device] = 0;
        publish(mouse_.release(device));
        mouse_active_.store(mouse_.active_mask());
        return;
    }
    const auto shortcut = mouse_shortcuts_[device].update(state.buttons, right, GetTickCount64());
    state.buttons = shortcut.buttons;
    if (shortcut.action == MouseShortcut::ToggleEnabled)
        change_mouse_options(MousePaused, 0, MouseEnabled);
    else if (shortcut.action == MouseShortcut::SwitchSource)
        change_mouse_options(MousePaused, 0, MouseGyro);
    auto options = mouse_options_.load();
    const bool enabled = (options & MouseEnabled) != 0;
    const bool gyro = (options & MouseGyro) != 0;
    const bool stick_down = (state.buttons & (right ? R3 : L3)) != 0;
    if (enabled && gyro && stick_down && !mouse_previous_stick_[device])
        options = change_mouse_options(0, 0, MousePaused);
    mouse_previous_stick_[device] = stick_down;
    const auto changed = uint8_t(options ^ mouse_previous_options_[device]);
    if (changed & (MouseEnabled | MouseGyro)) publish(mouse_.release(device));
    mouse_previous_options_[device] = options;

    auto sample = decode_joy_mouse(bytes).value_or(JoyMouseSample{});
    if (gyro) { sample.dx = sample.dy = 0; sample.surface = 1; }
    // Use the filtered logical buttons so a shortcut never also clicks, changes
    // sensitivity, or leaks its component buttons to the gamepad collections.
    sample.buttons = uint8_t(((state.buttons & (right ? R : L)) ? 1 : 0) |
                             ((state.buttons & (right ? ZR : ZL)) ? 2 : 0) |
                             ((state.buttons & (right ? X : Up)) ? 4 : 0));
    sample.sensitivity = gyro ? 0 : int8_t(int(right && (state.buttons & Home)) -
                                          int((state.buttons & (right ? Chat : Capture)) != 0));
    const unsigned stick = right ? 2 : 0;
    const float scroll = mouse_scroll_axis(state.sticks[stick + 1], calibration[stick + 1]);
    const bool ready = gyro ? gyro_ready : optical_ready;
    if (enabled && ready) mouse_ready_.fetch_or(bit);
    else mouse_ready_.fetch_and(uint8_t(~bit));
    publish(mouse_.update(device, sample, enabled && ready, scroll, GetTickCount64(), gyro));
    if (mouse_.active(device)) {
        uint32_t consumed = right ? (R | ZR | X) : (L | ZL | Up);
        if (!gyro) consumed |= right ? (Home | Chat) : Capture;
        if (gyro) consumed |= right ? R3 : L3;
        state.buttons &= ~consumed;
        state.sticks[stick] = calibration[stick].center;
        state.sticks[stick + 1] = calibration[stick + 1].center;
    }
    mouse_active_.store(mouse_.active_mask());
    mouse_sensitivity_.store(mouse_.sensitivity_level());
}
void JoySession::update(unsigned device, const State &state, const Calibration &calibration,
                        bool connected, bool right, bool physical_report) {
    std::array<JoyFrame, MaxJoyCons> outgoing;
    uint8_t changed;
    {
        std::lock_guard lock(mutex_);
        states_[device] = state;
        calibration_[device] = calibration;
        const bool refresh = refresh_.exchange(false);
        const bool topology = routing_.connect(device, connected, right) || refresh;
        if (topology)
            for (auto &motor : rumble_)
                motor.store(0);
        if (connected && physical_report)
            ++reports_[device];
        const auto status = routing_.status();
        if (topology)
            mouse_allowed_.store(joy_mouse_devices(status.connected, status.right_sides));
        if (!status.connected) change_mouse_options(MouseEnabled | MousePaused, 0);
        const auto current_slot = routing_.slot_for(device);
        if (connected && current_slot < MaxJoyCons)
            settings_[device].update(raw_states_[device], state, calibration, current_slot + 1,
                std::has_single_bit(unsigned(status.groups[current_slot])), 2);
        changed = topology ? 0x0f : current_slot < MaxJoyCons ? uint8_t(1u << current_slot) : 0;
        for (unsigned slot = 0; slot < MaxJoyCons; ++slot) {
            if (!(changed & (1u << slot)))
                continue;
            JoyFrame frame;
            frame.slot = uint8_t(slot);
            frame.sequence = ++sequence_;
            frame.updated_ms = GetTickCount64();
            frame.controls = xbox_controls(State{}, Calibration{});
            Motion motion;
            const auto group = status.groups[slot];
            if (group) {
                frame.flags = 1;
                if (std::has_single_bit(unsigned(group))) {
                    const unsigned source = std::countr_zero(unsigned(group));
                    const bool source_right = (status.right_sides & group) != 0;
                    frame.flags |= JoySideways;
                    if (source_right) frame.flags |= JoyRight;
                    frame.controls =
                        single_joy_controls(source_right, states_[source], calibration_[source]);
                    if (source == device)
                        motion = sideways_motion(source_right, states_[source].motion);
                } else {
                    frame.flags |= JoyRight;
                    const unsigned left_source =
                        std::countr_zero(unsigned(group & ~status.right_sides));
                    const unsigned right_source =
                        std::countr_zero(unsigned(group & status.right_sides));
                    frame.controls =
                        combined_joy_controls(states_[left_source], calibration_[left_source],
                                              states_[right_source], calibration_[right_source]);
                    // Only right-hand packets publish motion for a combined pair.
                    if (device == right_source)
                        motion = states_[right_source].motion;
                }
                if (group & mouse_allowed_.load()) {
                    frame.flags |= JoyMouseEligible;
                    const auto options = mouse_options_.load();
                    if ((options & (MouseEnabled | MouseGyro)) == (MouseEnabled | MouseGyro))
                        frame.flags |= JoyGyroMouse;
                    if (options & MousePaused) frame.flags |= JoyGyroPaused;
                }
                if (motion.ready) {
                    frame.flags |= 2;
                    frame.gyro = motion.angular_velocity;
                    frame.acceleration = motion.acceleration;
                    frame.sample_time_100ns = motion.sample_time_100ns;
                    // Native gamepad reports copy only controls[0..14]. Keep
                    // original IMU duration separate from Windows clock slew.
                    if (motion.sample_interval_100ns && motion.sample_interval_100ns % 12500 == 0 &&
                        motion.sample_interval_100ns / 12500 <= 255)
                        frame.controls[15] = uint8_t(motion.sample_interval_100ns / 12500);
                }
            }
            frames_[slot] = frame;
        }
        outgoing = frames_;
    }
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
        if (changed & (1u << slot))
            publish_(outgoing[slot]);
}
void JoySession::run(unsigned side, std::stop_token stop) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        return;
    }
    uint64_t previous_address = 0;
    bool initial_adoption = true;
    while (!stop.stop_requested()) {
        const auto config = configured_device(side);
        const auto address = config.address;
        if (address != previous_address) initial_adoption = true;
        previous_address = address;
        if (address) {
            owners_.fetch_or(uint8_t(1u << side));
            try {
                BleController controller(
                    {address, false,
                     config.right ? ControllerKind::JoyRight : ControllerKind::JoyLeft}, {
                        .wait_until_available = true,
                        .reuse_windows_connection = std::exchange(initial_adoption, false),
                        .cancelled = [&] { return stop.stop_requested() || configured_device(side).address != address; }});
                controller.initialize(false);
                std::vector<uint8_t> identity;
                try { identity = controller.read_memory(0x13000, 32); } catch (...) {}
                settings_[side].connect(config.right ? ControllerKind::JoyRight : ControllerKind::JoyLeft, identity, address);
                Calibration calibration;
                const auto user = controller.read_memory(0x1fc040, 11);
                const auto factory = controller.read_memory(0x130a8, 9);
                const auto data = user[0] == 0xb2 && user[1] == 0xa1 ? std::span(user).subspan(2)
                                                                     : std::span(factory);
                if (!parse_calibration(data, calibration, config.right ? 2 : 0))
                    throw std::runtime_error("Invalid Joy-Con stick calibration");
                std::optional<MotionProcessor> motion;
                const auto bias = parse_gyro_bias(controller.read_memory(0x13040, 16));
                if (bias) motion.emplace(*bias);
                std::optional<PackedMotionProcessor> packed_motion;
                if (bias) {
                    try {
                        const auto packed_config = parse_packed_motion_config(controller.motion_configuration());
                        if (packed_config) {
                            controller.enable_packed_motion();
                            packed_motion.emplace(*packed_config, *bias);
                        }
                    } catch (...) {
                        // A controller without the alternate format retains ordinary input.
                        OutputDebugStringW(L"Switch2Native: Joy-Con packed motion unavailable\n");
                    }
                }
                auto last_input = std::chrono::steady_clock::now(), last_output = last_input,
                     last_config = last_input;
                uint8_t sequence = 0;
                bool vibrating = false;
                uint8_t last_lights = 1;
                bool mouse_sensor = false, mouse_attempted = false;
                const auto device_bit = uint8_t(1u << side);
                try {
                    while (!stop.stop_requested()) {
                        const auto options = mouse_options_.load();
                        const bool requested_mouse = (options & MouseEnabled) && !(options & MouseGyro) &&
                                                     (mouse_allowed_.load() & device_bit);
                        if (requested_mouse != mouse_attempted) {
                            mouse_attempted = requested_mouse;
                            release_mouse(side);
                            mouse_ready_.fetch_and(uint8_t(~device_bit));
                            mouse_errors_.fetch_and(uint8_t(~device_bit));
                            try {
                                if (requested_mouse && !packed_motion)
                                    throw std::runtime_error("Mouse mode requires packed reports");
                                controller.set_mouse_sensor(requested_mouse);
                                mouse_sensor = requested_mouse;
                                if (mouse_sensor) mouse_ready_.fetch_or(device_bit);
                            } catch (...) {
                                mouse_sensor = false;
                                mouse_errors_.fetch_or(device_bit);
                            }
                        }
                        const auto bytes = controller.read(20);
                        const auto now = std::chrono::steady_clock::now();
                        if (auto state = decode_packed_joy_controls(bytes); state && packed_motion) {
                            state->profile = settings_[side].profile();
                            raw_states_[side] = *state;
                            FILETIME time;
                            GetSystemTimePreciseAsFileTime(&time);
                            const uint64_t arrival = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                            const auto host_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                            const size_t length_offset = bytes[0] == 8 ? 16 : 15;
                            std::optional<MotionBatch> batch;
                            if (bytes[length_offset] <= 40)
                                batch = packed_motion->process(std::span(bytes).subspan(length_offset + 1, bytes[length_offset]), host_us, arrival);
                            process_mouse(side, config.right, *state, calibration, bytes, mouse_sensor, true);
                            if (batch && batch->count) {
                                for (size_t i = 0; i < batch->count; ++i) {
                                    state->motion = batch->samples[i];
                                    process_gyro(side, state->motion);
                                    update(side, *state, calibration, true, config.right, i == 0);
                                }
                            } else {
                                update(side, *state, calibration, true, config.right);
                            }
                            last_input = now;
                        } else if (auto common_state = decode_ble(bytes)) {
                            common_state->profile = settings_[side].profile();
                            raw_states_[side] = *common_state;
                            if (motion)
                                common_state->motion = motion->process(common_state->raw_motion, now);
                            if (common_state->motion.ready) {
                                FILETIME time;
                                GetSystemTimePreciseAsFileTime(&time);
                                common_state->motion.sample_time_100ns = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                            }
                            process_mouse(side, config.right, *common_state, calibration, bytes, false, motion.has_value());
                            process_gyro(side, common_state->motion);
                            update(side, *common_state, calibration, true, config.right);
                            last_input = now;
                        }
                        if (now - last_input > std::chrono::seconds(2))
                            throw std::runtime_error("Joy-Con input stalled");
                        if (now - last_config > std::chrono::seconds(1)) {
                            last_config = now;
                            if (address != configured_device(side).address)
                                break;
                        }
                        if (now - last_output >= std::chrono::milliseconds(20)) {
                            uint8_t lights = 0;
                            {
                                std::lock_guard lock(mutex_);
                                const auto slot = routing_.slot_for(side);
                                lights = joy_player_lights(
                                    config.right, slot + 1,
                                    std::has_single_bit(unsigned(routing_.physical_mask(slot))));
                            }
                            if (lights != last_lights) {
                                controller.set_player_lights(lights);
                                last_lights = lights;
                            }
                            const auto motors = rumble_[side].load();
                            if (motors || vibrating) {
                                auto report =
                                    ble_rumble(sequence++, uint8_t(motors), uint8_t(motors >> 8));
                                controller.write(std::span(report).first(17));
                            }
                            vibrating = motors != 0;
                            last_output = now;
                        }
                    }
                } catch (...) {
                    release_mouse(side, true);
                    if (mouse_sensor) {
                        try { controller.set_mouse_sensor(false); } catch (...) {}
                    }
                    if (vibrating) {
                        try {
                            auto report = ble_rumble(sequence, 0, 0);
                            controller.write(std::span(report).first(17));
                        } catch (...) {
                        }
                    }
                    throw;
                }
                release_mouse(side, true);
                if (mouse_sensor) controller.set_mouse_sensor(false);
                if (vibrating) {
                    auto report = ble_rumble(sequence, 0, 0);
                    controller.write(std::span(report).first(17));
                }
            } catch (...) {
                std::lock_guard lock(mutex_);
                ++errors_[side];
            }
        }
        owners_.fetch_and(uint8_t(~(1u << side)));
        mouse_ready_.fetch_and(uint8_t(~(1u << side)));
        mouse_errors_.fetch_and(uint8_t(~(1u << side)));
        release_mouse(side, true);
        settings_[side].disconnect();
        update(side, State{}, Calibration{}, false);
        rumble_[side].store(0);
        for (unsigned i = 0; i < 10 && !stop.stop_requested(); ++i)
            Sleep(100);
    }
    winrt::uninit_apartment();
}
} // namespace switch2
