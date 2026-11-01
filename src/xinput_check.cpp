#include "windows_support.hpp"
#include "hid_helpers.hpp"
#include <Xinput.h>
#include <hidsdi.h>
#include <array>
#include <cstring>
#include <iomanip>

static void driver_status() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    for (const auto &path : switch2::device_paths(guid)) {
        switch2::Handle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, 0, nullptr));
        if (!handle.valid())
            continue;
        WCHAR serial[128]{};
        if (!HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial)) ||
            std::wstring(serial) != L"Switch2Native-01" ||
            !switch2::has_hid_usage(handle.get(), 1, 5))
            continue;
        uint8_t report[64]{0xe1};
        if (!HidD_GetFeature(handle.get(), report, sizeof(report)))
            switch2::fail("Read driver diagnostics");
        uint32_t reports = 0, errors = 0;
        std::memcpy(&reports, report + 4, 4);
        std::memcpy(&errors, report + 8, 4);
        report[59] = 0;
        std::cout << "Driver transport=" << unsigned(report[2]) << " reports=" << reports
                  << " errors=" << errors << " last=" << reinterpret_cast<char *>(report + 12)
                  << '\n';
        return;
    }
    std::cout << "Switch2Native HID diagnostic interface not found\n";
}
int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        unsigned seconds = 5, slot = 0;
        bool rumble = false;
        for (int i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (argument == "--slot" && i + 1 < argc)
                slot = std::stoul(argv[++i]);
            else if (argument == "--rumble")
                rumble = true;
            else
                throw std::runtime_error(
                    "Usage: XInputCheck [--seconds N] [--slot 0..3] [--rumble]");
        }
        if (slot >= XUSER_MAX_COUNT)
            throw std::runtime_error("Invalid XInput slot");
        driver_status();
        std::array<bool, 4> connected{};
        std::array<XINPUT_STATE, 4> previous{};
        std::array<unsigned, 4> changes{};
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        bool first = true;
        while (std::chrono::steady_clock::now() < until) {
            for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
                XINPUT_STATE state{};
                bool available = XInputGetState(index, &state) == ERROR_SUCCESS;
                if (first || available != connected[index])
                    std::cout << "slot=" << index << " connected=" << available << '\n';
                connected[index] = available;
                if (available && (first || std::memcmp(&state.Gamepad, &previous[index].Gamepad,
                                                       sizeof(XINPUT_GAMEPAD)))) {
                    ++changes[index];
                    std::cout << "tick=" << GetTickCount64() << " slot=" << index << " buttons=0x" << std::hex
                              << state.Gamepad.wButtons << std::dec
                              << " LT=" << unsigned(state.Gamepad.bLeftTrigger)
                              << " RT=" << unsigned(state.Gamepad.bRightTrigger)
                              << " axes=" << state.Gamepad.sThumbLX << ',' << state.Gamepad.sThumbLY
                              << ',' << state.Gamepad.sThumbRX << ',' << state.Gamepad.sThumbRY
                              << '\n';
                    previous[index] = state;
                }
            }
            if (first && rumble) {
                XINPUT_VIBRATION vibration{8192, 8192};
                auto status = XInputSetState(slot, &vibration);
                if (status != ERROR_SUCCESS)
                    throw std::runtime_error("XInput rumble failed: " + std::to_string(status));
                Sleep(300);
                vibration = {};
                XInputSetState(slot, &vibration);
            }
            first = false;
            Sleep(8);
        }
        for (unsigned index = 0; index < 4; ++index)
            if (changes[index])
                std::cout << "slot=" << index << " state_changes=" << changes[index] << '\n';
        driver_status();
        return std::any_of(connected.begin(), connected.end(), [](bool value) { return value; })
                   ? 0
                   : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
