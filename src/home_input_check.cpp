#define DIRECTINPUT_VERSION 0x0800
#include "hid_helpers.hpp"
#include <dinput.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace {
struct Search {
    IDirectInput8W *input;
    std::wstring serial;
    ComPtr<IDirectInputDevice8W> device;
};
BOOL CALLBACK find_home(const DIDEVICEINSTANCEW *instance, void *context) {
    auto &search = *static_cast<Search *>(context);
    ComPtr<IDirectInputDevice8W> device;
    if (FAILED(search.input->CreateDevice(instance->guidInstance, &device, nullptr))) return DIENUM_CONTINUE;
    DIPROPGUIDANDPATH path{};
    path.diph = {sizeof(path), sizeof(path.diph), 0, DIPH_DEVICE};
    if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph))) return DIENUM_CONTINUE;
    switch2::Handle handle(CreateFileW(path.wszPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr));
    WCHAR serial[128]{};
    if (!handle.valid() || !switch2::has_hid_usage(handle.get(), 1, 4) ||
        !HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial)) || search.serial != serial)
        return DIENUM_CONTINUE;
    std::wcout << L"DirectInput device=" << instance->tszProductName << L" serial=" << serial << L'\n';
    search.device = std::move(device);
    return DIENUM_STOP;
}
void checked(HRESULT result, const char *operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        const unsigned seconds = argc >= 2 ? std::stoul(argv[1]) : 60;
        const unsigned slot = argc >= 3 ? std::stoul(argv[2]) : 0;
        if (argc > 3 || !seconds || seconds > 600 || slot > 4)
            throw std::runtime_error("Usage: HomeInputCheck [seconds 1..600] [Joy-Con slot 1..4]");
        ComPtr<IDirectInput8W> input;
        checked(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                  IID_IDirectInput8W, reinterpret_cast<void **>(input.GetAddressOf()), nullptr),
                "Create DirectInput");
        Search search{input.Get(), slot ? L"Switch2Native-JoyMotion-0" + std::to_wstring(slot)
                                       : L"Switch2Native-Motion-01"};
        const auto discovery_deadline = GetTickCount64() + 60000;
        std::cout << "Waiting up to 60 seconds for the Home joystick...\n";
        do {
            checked(input->EnumDevices(DI8DEVCLASS_GAMECTRL, find_home, &search, DIEDFL_ATTACHEDONLY), "Find Home joystick");
            if (!search.device) Sleep(250);
        } while (!search.device && GetTickCount64() < discovery_deadline);
        if (!search.device) throw std::runtime_error("Home joystick not enumerated by DirectInput");
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        checked(search.device->GetCapabilities(&caps), "Get capabilities");
        std::cout << "axes=" << caps.dwAxes << " buttons=" << caps.dwButtons << '\n';
        checked(search.device->SetDataFormat(&c_dfDIJoystick2), "Set joystick format");
        checked(search.device->SetCooperativeLevel(nullptr, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE), "Set background input");
        checked(search.device->Acquire(), "Acquire Home joystick");
        const auto start = GetTickCount64();
        int previous = -1;
        unsigned presses = 0, releases = 0;
        while (GetTickCount64() - start < uint64_t(seconds) * 1000) {
            search.device->Poll();
            DIJOYSTATE2 state{};
            checked(search.device->GetDeviceState(sizeof(state), &state), "Read Home button");
            const int pressed = (state.rgbButtons[0] & 0x80) != 0;
            if (pressed != previous) {
                std::cout << "time_ms=" << GetTickCount64() - start << " DirectInput home_button=" << pressed << '\n';
                if (pressed) ++presses;
                else if (previous == 1) ++releases;
                previous = pressed;
            }
            Sleep(4);
        }
        search.device->Unacquire();
        std::cout << "presses=" << presses << " releases=" << releases << '\n';
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
