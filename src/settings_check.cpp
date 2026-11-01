#include "joy_ipc.hpp"
#include "controller_button_layout.hpp"
#include <iostream>
#include <thread>
#include <iomanip>

using namespace switch2;
namespace ipc = joy_ipc;
int main(int argc, char **argv) {
    try {
        ipc::Request request;
        request.command = ipc::Command::SettingsRead;
        ipc::SettingsResponse first;
        if (!ipc::exchange(request, first))
            throw std::runtime_error("Settings service unavailable");
        if (argc == 7 && std::string(argv[1]) == "--set") {
            request.command = ipc::Command::SettingsSave;
            request.mode = std::stoul(argv[2]);
            request.generation = first.generation;
            ControllerProfile profile;
            const unsigned deadzone = std::stoul(argv[3]), circular = std::stoul(argv[4]),
                           gain = std::stoul(argv[5]), stick = std::stoul(argv[6]);
            if (request.mode >= 5 || deadzone > 40 || circular > 1 || gain < 10 || gain > 360 ||
                stick > 1)
                throw std::runtime_error("Invalid profile arguments");
            profile = first.controllers[request.mode].profile;
            profile.deadzone[stick] = uint8_t(deadzone);
            profile.circular_mask =
                uint8_t((profile.circular_mask & ~(1 << stick)) | (circular << stick));
            profile.gyro_counts_per_degree = uint16_t(gain);
            request.pair = ipc::pack_profile(profile);
            if (!ipc::exchange(request, first) || first.result)
                throw std::runtime_error("Settings save failed: " + std::to_string(first.result));
            std::cout << "Saved profile\n";
        } else if (argc > 1 && !(argc == 3 && std::string(argv[1]) == "--seconds")) {
            std::cerr << "Usage: SettingsCheck [--seconds N | --set index deadzone circular gain "
                         "stick]\n";
            return 2;
        }
        for (unsigned i = 0; i < 5; ++i) {
            const auto &c = first.controllers[i];
            if (!c.connected)
                continue;
            std::cout << "index=" << i << " kind=" << std::hex << unsigned(c.kind) << std::dec
                      << " player=" << unsigned(c.player) << " transport=" << unsigned(c.transport)
                      << " identity=" << unsigned(c.identity_source) << ":";
            for (auto ch : c.serial) {
                if (!ch)
                    break;
                std::cout << ch;
            }
            std::cout << " deadzones=" << unsigned(c.profile.deadzone[0]) << ","
                      << unsigned(c.profile.deadzone[1])
                      << " circular=" << unsigned(c.profile.circular_mask)
                      << " gain=" << c.profile.gyro_counts_per_degree
                      << " button_data=" << unsigned(c.diagnostic_flags & 1) << " held=0x"
                      << std::hex << c.buttons << std::dec << '\n';
        }
        if (argc == 3) {
            const auto seconds = std::clamp(std::stoi(argv[2]), 1, 180);
            const auto start = GetTickCount64();
            ipc::SettingsResponse last = first;
            std::array<bool, 5> continuous;
            for (unsigned i = 0; i < 5; ++i)
                continuous[i] = first.controllers[i].connected != 0;
            std::array<std::array<int16_t, 4>, 5> minimum{}, maximum{};
            std::array<std::array<float, 3>, 5> gyro_max{};
            std::array<uint32_t, 5> seen_buttons{}, held_buttons{}, released_buttons{};
            unsigned failures = 0;
            while (GetTickCount64() - start < uint64_t(seconds) * 1000) {
                ipc::SettingsResponse next;
                request.command = ipc::Command::SettingsRead;
                if (ipc::exchange(request, next)) {
                    for (unsigned i = 0; i < 5; ++i) {
                        continuous[i] = continuous[i] && next.controllers[i].connected &&
                                        next.controllers[i].serial == first.controllers[i].serial &&
                                        next.motion[i].samples >= last.motion[i].samples;
                        const auto preview = model_button_state(next.controllers[i],
                                                                uint32_t(GetTickCount64()), true);
                        seen_buttons[i] |= preview.lit;
                        held_buttons[i] |= preview.held;
                        released_buttons[i] |= last.controllers[i].buttons & ~preview.held;
                        for (unsigned a = 0; a < 4; ++a) {
                            minimum[i][a] =
                                std::min(minimum[i][a], next.controllers[i].raw_sticks[a]);
                            maximum[i][a] =
                                std::max(maximum[i][a], next.controllers[i].raw_sticks[a]);
                        }
                        for (unsigned a = 0; a < 3; ++a)
                            gyro_max[i][a] =
                                std::max(gyro_max[i][a], std::abs(next.motion[i].gyro[a]));
                    }
                    last = next;
                } else
                    ++failures;
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            const double elapsed = (GetTickCount64() - start) / 1000.0;
            for (unsigned i = 0; i < 5; ++i)
                if (last.controllers[i].connected || seen_buttons[i]) {
                    std::cout << "index=" << i << " hz=";
                    if (continuous[i])
                        std::cout << (last.motion[i].samples - first.motion[i].samples) / elapsed;
                    else
                        std::cout << "unavailable(connection changed)";
                    std::cout << " sticks=";
                    for (unsigned a = 0; a < 4; ++a)
                        std::cout << minimum[i][a] << ":" << maximum[i][a] << " ";
                    std::cout << "gyro_max=";
                    for (auto v : gyro_max[i])
                        std::cout << v << " ";
                    std::cout << "q=";
                    for (auto v : last.motion[i].orientation)
                        std::cout << v << " ";
                    std::cout << " buttons_seen=0x" << std::hex << seen_buttons[i]
                              << " held_seen=0x" << held_buttons[i] << " released=0x"
                              << released_buttons[i] << " final_held=0x"
                              << last.controllers[i].buttons << std::dec << '\n';
                }
            std::cout << "IPC failures=" << failures << '\n';
        }
        return first.result ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
