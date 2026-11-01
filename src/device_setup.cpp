// SPDX-License-Identifier: GPL-3.0-only
// Root-device creation for the offline installer. No WDK redistributables required.
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cfgmgr32.h>
#include <array>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::array<const wchar_t*, 17> hardwareIds = {
    L"ROOT\\SWITCH2NATIVE", L"ROOT\\SWITCH2MOTION", L"ROOT\\SWITCH2PROHUB", L"ROOT\\SWITCH2JOYHUB",
    L"ROOT\\SWITCH2JOYPAD1", L"ROOT\\SWITCH2JOYPAD2", L"ROOT\\SWITCH2JOYPAD3", L"ROOT\\SWITCH2JOYPAD4",
    L"ROOT\\SWITCH2JOYMOTION1", L"ROOT\\SWITCH2JOYMOTION2", L"ROOT\\SWITCH2JOYMOTION3", L"ROOT\\SWITCH2JOYMOTION4",
    L"ROOT\\SWITCH2PROSENSOR", L"ROOT\\SWITCH2JOYSENSOR1", L"ROOT\\SWITCH2JOYSENSOR2",
    L"ROOT\\SWITCH2JOYSENSOR3", L"ROOT\\SWITCH2JOYSENSOR4"
};
struct DeviceSet {
    HDEVINFO value = INVALID_HANDLE_VALUE;
    ~DeviceSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};
void check(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) + " failed (Windows error " +
                                         std::to_string(GetLastError()) + ").");
}
bool allowed(const std::wstring& id) {
    for (auto candidate : hardwareIds) if (id == candidate) return true;
    return false;
}
GUID validateInf(const std::wstring& path, const std::wstring& id) {
    if (!allowed(id)) throw std::runtime_error("Only Switch2Native virtual hardware IDs are allowed.");
    GUID actual{};
    wchar_t className[MAX_CLASS_NAME_LEN]{};
    check(SetupDiGetINFClassW(path.c_str(), &actual, className, MAX_CLASS_NAME_LEN, nullptr), "Read INF class");
    const GUID hid = {0x745a17a0, 0x74d3, 0x11d0, {0xb6,0xfe,0x00,0xa0,0xc9,0x0f,0x57,0xda}};
    const GUID sensor = {0x5175d334, 0xc371, 0x4806, {0xb3,0xba,0x71,0xfd,0x53,0xc9,0x25,0x8d}};
    const bool isSensor = id.find(L"SENSOR") != std::wstring::npos;
    if (!IsEqualGUID(actual, isSensor ? sensor : hid)) throw std::runtime_error("Unexpected INF class.");
    const auto inf = SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    check(inf != INVALID_HANDLE_VALUE, "Open INF");
    INFCONTEXT line{};
    bool found = false;
    if (SetupFindFirstLineW(inf, L"Devices.NTamd64.10.0...22000", nullptr, &line)) {
        do {
            wchar_t modelId[256]{};
            if (SetupGetStringFieldW(&line, 2, modelId, 256, nullptr) && id == modelId) found = true;
        } while (SetupFindNextLine(&line, &line));
    }
    SetupCloseInfFile(inf);
    if (!found) throw std::runtime_error("Hardware ID is absent from the INF model list.");
    return actual;
}
void rejectDuplicate(const std::wstring& id) {
    DeviceSet devices{SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
    check(devices.value != INVALID_HANDLE_VALUE, "Enumerate devices");
    SP_DEVINFO_DATA device{sizeof(SP_DEVINFO_DATA)};
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.value, index, &device); ++index) {
        DWORD bytes = 0;
        SetupDiGetDeviceRegistryPropertyW(devices.value, &device, SPDRP_HARDWAREID, nullptr, nullptr, 0, &bytes);
        if (!bytes) continue;
        std::vector<wchar_t> ids(bytes / sizeof(wchar_t) + 2, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(devices.value, &device, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<BYTE*>(ids.data()), bytes, nullptr)) continue;
        for (auto value = ids.data(); *value; value += wcslen(value) + 1)
            if (_wcsicmp(value, id.c_str()) == 0) throw std::runtime_error("Virtual device already exists; use the updater.");
    }
    check(GetLastError() == ERROR_NO_MORE_ITEMS, "Enumerate device list");
}
int install(const std::wstring& path, const std::wstring& id, const GUID& classId) {
    if (!IsUserAnAdmin()) throw std::runtime_error("Administrator access is required.");
    rejectDuplicate(id);
    DeviceSet devices{SetupDiCreateDeviceInfoList(&classId, nullptr)};
    check(devices.value != INVALID_HANDLE_VALUE, "Create device set");
    SP_DEVINFO_DATA device{sizeof(SP_DEVINFO_DATA)};
    check(SetupDiCreateDeviceInfoW(devices.value, L"Switch2Native", &classId, nullptr, nullptr,
                                  DICD_GENERATE_ID, &device), "Create virtual device");
    std::vector<wchar_t> multiId(id.begin(), id.end());
    multiId.push_back(0);
    multiId.push_back(0);
    check(SetupDiSetDeviceRegistryPropertyW(devices.value, &device, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(multiId.data()), static_cast<DWORD>(multiId.size() * sizeof(wchar_t))),
          "Set hardware ID");
    check(SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices.value, &device), "Register virtual device");
    // Leave a registered node on failure so the journal-driven uninstaller can find
    // and remove it. Never remove a device created by a different setup process.
    BOOL restart = FALSE;
    check(UpdateDriverForPlugAndPlayDevicesW(nullptr, id.c_str(), path.c_str(), 0, &restart), "Install driver");
    SP_DEVINSTALL_PARAMS_W parameters{sizeof(SP_DEVINSTALL_PARAMS_W)};
    check(SetupDiGetDeviceInstallParamsW(devices.value, &device, &parameters), "Read restart state");
    restart = restart || (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART));
    std::wcout << L"Installed " << id << L"; restart required: " << restart << L'\n';
    return restart ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4 || (wcscmp(argv[1], L"install") != 0 && wcscmp(argv[1], L"check") != 0)) {
            std::wcerr << L"Usage: Switch2DeviceSetup (check|install) <absolute INF path> <hardware ID>\n";
            return 2;
        }
        const std::wstring path = argv[2], id = argv[3];
        if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') throw std::runtime_error("An absolute local INF path is required.");
        const auto classId = validateInf(path, id);
        if (wcscmp(argv[1], L"check") == 0) return 0;
        return install(path, id, classId);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
