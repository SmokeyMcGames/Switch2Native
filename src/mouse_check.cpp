#include "joy_ipc.hpp"
#include "hid_helpers.hpp"
#include <map>
#include <vector>

namespace {
std::map<HANDLE, bool> devices;
unsigned reports{}, left_down{}, left_up{}, right_down{}, right_up{};
unsigned middle_down{}, middle_up{};
unsigned wheel_up{}, wheel_down{};
bool is_switch2_mouse(HANDLE device) {
    if (const auto found = devices.find(device); found != devices.end()) return found->second;
    UINT size = 0;
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size);
    std::wstring path(size, L'\0');
    if (!size || GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path.data(), &size) == UINT(-1))
        return devices[device] = false;
    switch2::Handle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr));
    wchar_t serial[128]{};
    const bool matched = handle.valid() && HidD_GetSerialNumberString(handle.get(), serial, sizeof(serial)) &&
                         std::wstring_view(serial) == L"Switch2Native-JoyHub";
    if (matched) std::wcout << L"native_mouse=" << path.c_str() << L'\n';
    return devices[device] = matched;
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(HRAWINPUT(lparam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            input.header.dwType == RIM_TYPEMOUSE && is_switch2_mouse(input.header.hDevice)) {
            const auto &mouse = input.data.mouse;
            const auto flags = mouse.usButtonFlags;
            ++reports;
            left_down += (flags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0;
            left_up += (flags & RI_MOUSE_LEFT_BUTTON_UP) != 0;
            right_down += (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0;
            right_up += (flags & RI_MOUSE_RIGHT_BUTTON_UP) != 0;
            middle_down += (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0;
            middle_up += (flags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0;
            const auto wheel = (flags & RI_MOUSE_WHEEL) ? int16_t(mouse.usButtonData) : 0;
            wheel_up += wheel > 0;
            wheel_down += wheel < 0;
            std::cout << "mouse tick=" << GetTickCount64() << " dx=" << mouse.lLastX << " dy=" << mouse.lLastY
                      << " absolute=" << ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) << " buttons=" << flags << " wheel=" << wheel << '\n';
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int main(int argc, char **argv) {
    try {
        const unsigned seconds = argc == 2 ? std::stoul(argv[1]) : 60;
        if (!seconds || seconds > 180) throw std::invalid_argument("Usage: MouseCheck [1-180 seconds]");
        std::cout << std::unitbuf;
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = window_proc;
        type.lpszClassName = L"Switch2Native.MouseCheck";
        if (!RegisterClassW(&type)) switch2::fail("Register MouseCheck");
        HWND window = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
        if (!window) switch2::fail("Create MouseCheck");
        RAWINPUTDEVICE device{1, 2, RIDEV_INPUTSINK, window};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) switch2::fail("Register mouse input");
        UINT device_count = 0;
        GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST));
        std::vector<RAWINPUTDEVICELIST> device_list(device_count);
        if (GetRawInputDeviceList(device_list.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) != UINT(-1))
            for (const auto &entry : device_list)
                if (entry.dwType == RIM_TYPEMOUSE) is_switch2_mouse(entry.hDevice);
        std::cout << "Listening to the Switch2Native native mouse; other mice are excluded\n";
        const auto until = GetTickCount64() + seconds * 1000ull;
        uint64_t next_status = 0;
        while (GetTickCount64() < until) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (GetTickCount64() >= next_status) {
                switch2::joy_ipc::Response response;
                if (switch2::joy_ipc::exchange({}, response)) {
                    const auto &s = response.status;
                    std::cout << "status tick=" << GetTickCount64() << " connected=" << unsigned(s.connected)
                              << " enabled=" << unsigned(s.mouse_enabled) << " ready=" << unsigned(s.mouse_ready)
                              << " active=" << unsigned(s.mouse_active) << " errors=" << unsigned(s.mouse_errors)
                              << " sensitivity=" << unsigned(s.mouse_sensitivity) * 25
                              << " source=" << unsigned(s.mouse_source) << " paused=" << unsigned(s.gyro_paused) << '\n';
                }
                next_status = GetTickCount64() + 500;
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_RAWINPUT);
        }
        DestroyWindow(window);
        std::cout << "summary reports=" << reports << " left_down=" << left_down << " left_up=" << left_up
                  << " right_down=" << right_down << " right_up=" << right_up << '\n';
        std::cout << "wheel_up=" << wheel_up << " wheel_down=" << wheel_down << '\n';
        std::cout << "middle_down=" << middle_down << " middle_up=" << middle_up << '\n';
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
