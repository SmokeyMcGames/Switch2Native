#include "hid_helpers.hpp"
#include "joy_protocol.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <iomanip>
#include <cstring>

namespace {
std::mutex output_mutex;
void inspect_physical() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    for (const auto &path : switch2::device_paths(guid)) {
        switch2::Handle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, 0, nullptr));
        WCHAR serial[128]{};
        if (handle.valid()) HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial));
        if (!switch2::is_controller(path) && std::wstring(serial).find(L"Switch2Native-") != 0) continue;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!handle.valid() || !HidD_GetPreparsedData(handle.get(), &preparsed)) continue;
        HIDP_CAPS caps{};
        HidP_GetCaps(preparsed, &caps);
        std::wcout << L"interface serial=" << serial << L" path=" << path << L'\n';
        std::cout << "interface usage=" << std::hex << caps.UsagePage << ':' << caps.Usage
                  << std::dec << " input_length=" << caps.InputReportByteLength << '\n';
        std::vector<HIDP_BUTTON_CAPS> buttons(caps.NumberInputButtonCaps);
        USHORT count = USHORT(buttons.size());
        if (HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, preparsed) == HIDP_STATUS_SUCCESS)
            for (unsigned i = 0; i < count; ++i) {
                const auto &button = buttons[i];
                std::cout << "interface button report=" << unsigned(button.ReportID) << " page=" << std::hex
                          << button.UsagePage << " usage=" << (button.IsRange ? button.Range.UsageMin : button.NotRange.Usage)
                          << ".." << (button.IsRange ? button.Range.UsageMax : button.NotRange.Usage) << std::dec << '\n';
            }
        HidD_FreePreparsedData(preparsed);
    }
}
void watch(bool source, HANDLE stop, uint64_t start, unsigned joy_slot) {
    const auto serial = source ? (joy_slot ? L"Switch2Native-JoyHub" : L"Switch2Native-ProHub") :
        joy_slot ? L"Switch2Native-JoyMotion-0" + std::to_wstring(joy_slot) : L"Switch2Native-Motion-01";
    auto handle = switch2::open_hid_collection(source ? 0xff00 : 1, source ? 3 : 4,
        serial, GENERIC_READ);
    switch2::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!handle.valid() || !event.valid()) throw std::runtime_error("Guide diagnostic interface unavailable");
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(handle.get(), &preparsed)) switch2::fail("Guide report caps");
    HIDP_CAPS caps{};
    HidP_GetCaps(preparsed, &caps);
    if (!source) {
        std::vector<HIDP_BUTTON_CAPS> buttons(caps.NumberInputButtonCaps);
        USHORT count = USHORT(buttons.size());
        if (HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, preparsed) == HIDP_STATUS_SUCCESS) {
            std::lock_guard lock(output_mutex);
            for (unsigned i = 0; i < count; ++i) {
                const auto &button = buttons[i];
                std::cout << "virtual button report=" << unsigned(button.ReportID) << " page=" << std::hex
                          << button.UsagePage << " usage=" << (button.IsRange ? button.Range.UsageMin : button.NotRange.Usage)
                          << ".." << (button.IsRange ? button.Range.UsageMax : button.NotRange.Usage) << std::dec << '\n';
            }
        }
    }
    std::vector<uint8_t> bytes(caps.InputReportByteLength);
    HidD_SetNumInputBuffers(handle.get(), 128);
    int previous = -1;
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        if (!switch2::read_hid_packet(handle.get(), event.get(), stop, bytes.data(), DWORD(bytes.size()))) {
            if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) break;
            if (!source) continue; // Gamepad reports are sent only when controls change.
            std::lock_guard lock(output_mutex);
            std::cout << "time_ms=" << GetTickCount64() - start << " source stream timeout\n";
            continue;
        }
        int guide = -1;
        unsigned flags = 0;
        if (source && bytes.size() == sizeof(switch2::JoyFrame) && bytes[0] == 0xe6) {
            switch2::JoyFrame frame;
            std::memcpy(&frame, bytes.data(), sizeof(frame));
            if (joy_slot && frame.slot != joy_slot - 1) continue;
            guide = (frame.controls[14] & 4) != 0;
            if (frame.controls[15]) std::cout << "UNEXPECTED legacy Guide state\n";
            flags = frame.flags;
        } else if (!source) {
            USAGE usages[32]{};
            ULONG count = 32;
            if (HidP_GetUsages(HidP_Input, 9, 0, usages, &count, preparsed,
                              reinterpret_cast<PCHAR>(bytes.data()), ULONG(bytes.size())) == HIDP_STATUS_SUCCESS) {
                guide = std::find(usages, usages + count, USAGE(1)) != usages + count;
            }
        }
        if (guide >= 0 && guide != previous) {
            std::lock_guard lock(output_mutex);
            std::cout << "time_ms=" << GetTickCount64() - start << ' ' << (source ? "source" : "virtual")
                      << " home_button=" << guide << " flags=" << flags << '\n';
            previous = guide;
        }
    }
    HidD_FreePreparsedData(preparsed);
}
}
int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        const unsigned seconds = argc >= 2 ? std::stoul(argv[1]) : 60;
        const unsigned joy_slot = argc >= 3 ? std::stoul(argv[2]) : 0;
        if (argc > 3 || joy_slot > 4 || !seconds || seconds > 600)
            throw std::runtime_error("Usage: GuideCheck [seconds 1..600] [Joy-Con slot 1..4]");
        inspect_physical();
        switch2::Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        const auto start = GetTickCount64();
        std::atomic<bool> failed{};
        auto run = [&](bool source) {
            try { watch(source, stop.get(), start, joy_slot); }
            catch (const std::exception &error) { std::lock_guard lock(output_mutex); std::cerr << error.what() << '\n'; failed = true; }
        };
        std::jthread physical([&] { run(true); }), virtual_controller([&] { run(false); });
        Sleep(seconds * 1000);
        SetEvent(stop.get());
        physical.join(); virtual_controller.join();
        return failed ? 1 : 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
