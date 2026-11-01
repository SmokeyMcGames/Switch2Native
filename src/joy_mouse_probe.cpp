#include "ble.hpp"
#include <winrt/base.h>
#include <iomanip>

int main(int argc, char **argv) {
    try {
        if (argc != 4 || (std::string(argv[2]) != "left" && std::string(argv[2]) != "right"))
            throw std::invalid_argument("Usage: JoyMouseProbe ADDRESS left|right SECONDS");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::cout << std::unitbuf;
        const auto address = std::stoull(argv[1], nullptr, 16);
        const auto kind = std::string(argv[2]) == "right" ? switch2::ControllerKind::JoyRight : switch2::ControllerKind::JoyLeft;
        const auto seconds = std::stoul(argv[3]);
        if (!seconds || seconds > 120) throw std::invalid_argument("Capture duration must be 1-120 seconds");
        const auto deadline = GetTickCount64() + (seconds + 120) * 1000ull;
        switch2::BleController controller({address, false, kind}, {
            .wait_until_available = true, .reuse_windows_connection = true,
            .cancelled = [deadline] { return GetTickCount64() >= deadline; },
            .trace = [](const auto &message) { std::cerr << message << std::endl; }});
        controller.initialize(false);
        controller.enable_packed_motion();
        controller.set_mouse_sensor(true);
        try {
            std::cout << "tick_ms,counter,dx,dy,surface,buttons\n";
            const auto until = GetTickCount64() + seconds * 1000ull;
            while (GetTickCount64() < until) {
                const auto bytes = controller.read(100);
                if (bytes.size() != 64 || (bytes[0] != 7 && bytes[0] != 8)) continue;
                const auto signed_word = [&](size_t offset) { return int16_t(bytes[offset] | (uint16_t(bytes[offset + 1]) << 8)); };
                std::cout << GetTickCount64() << ',' << unsigned(bytes[1]) << ',' << signed_word(10)
                          << ',' << signed_word(12) << ',' << unsigned(bytes[14]) << ','
                          << unsigned(bytes[3] | (uint16_t(bytes[4]) << 8)) << '\n';
            }
        } catch (...) {
            try { controller.set_mouse_sensor(false); } catch (...) {}
            throw;
        }
        controller.set_mouse_sensor(false);
        return 0;
    } catch (const winrt::hresult_error &error) { std::cerr << winrt::to_string(error.message()) << '\n'; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
}
