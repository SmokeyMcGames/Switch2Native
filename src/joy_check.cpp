#include "joy_ipc.hpp"
#include "hid_helpers.hpp"
#include "motion_snapshot.hpp"
#include <iomanip>

int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        unsigned seconds = 1;
        bool pro_hub = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (std::string(argv[i]) == "--pro-hub")
                pro_hub = true;
            else
                throw std::runtime_error("Usage: JoyCheck [--seconds N] [--pro-hub]");
        }
        const auto until = GetTickCount64() + seconds * 1000ull;
        do {
            switch2::joy_ipc::Response response;
            auto hub = switch2::open_hid_collection(0xff00, 3, pro_hub ? L"Switch2Native-ProHub" : L"Switch2Native-JoyHub", 0);
            if (pro_hub) {
                if (!hub.valid() || !HidD_GetFeature(hub.get(), &response.status, sizeof(response.status)))
                    throw std::runtime_error("Pro transport coordinator unavailable");
                response.available = 1;
            } else if (!switch2::joy_ipc::exchange({}, response))
                throw std::runtime_error("Joy-Con manager unavailable");
            const auto &s = response.status;
            std::cout << "available=" << response.available
                      << " connected=" << unsigned(s.connected) << " mode=" << unsigned(s.mode)
                      << " active=" << unsigned(s.active_slots)
                      << " enabled=" << response.enabled_slots
                      << " device_error=" << response.device_error << " generation=" << s.generation
                      << " pending=" << unsigned(s.needs_choice) << " reports=" << s.reports[0]
                      << ',' << s.reports[1] << " errors=" << s.errors[0] << ',' << s.errors[1]
                      << " owners=" << unsigned(s.owners)
                      << " mouse_enabled=" << unsigned(s.mouse_enabled)
                      << " mouse_ready=" << unsigned(s.mouse_ready)
                      << " mouse_active=" << unsigned(s.mouse_active)
                      << " mouse_errors=" << unsigned(s.mouse_errors)
                      << " mouse_sensitivity=" << unsigned(s.mouse_sensitivity) * 25
                      << " mouse_source=" << unsigned(s.mouse_source) << " gyro_paused=" << unsigned(s.gyro_paused)
                      << '\n';
            if (hub.valid())
                for (unsigned slot = 0; slot < (pro_hub ? 1 : switch2::MaxJoyCons); ++slot) {
                    switch2::JoyFrame frame;
                    frame.id = switch2::JoySnapshotIds[slot];
                    if (HidD_GetFeature(hub.get(), &frame, sizeof(frame))) {
                        std::cout << "logical=" << slot << " flags=" << unsigned(frame.flags)
                                  << " sequence=" << frame.sequence
                                  << " age_ms=" << GetTickCount64() - frame.updated_ms
                                  << " controls=";
                        for (auto byte : frame.controls)
                            std::cout << std::hex << std::setfill('0') << std::setw(2)
                                      << unsigned(byte);
                        std::cout << std::dec << " gyro=" << frame.gyro[0] << ',' << frame.gyro[1]
                                  << ',' << frame.gyro[2] << '\n';
                    }
                }
            if (GetTickCount64() < until)
                Sleep(1000);
        } while (GetTickCount64() < until);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
