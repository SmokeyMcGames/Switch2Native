#pragma once
#include "windows_support.hpp"
#include <hidsdi.h>

namespace switch2 {
// Release cross-device HID handles during a package update without removing the
// consumers' XInput/sensor nodes. A crashed updater cannot pause input forever.
class MaintenancePause {
  public:
    bool active() {
        const auto now = GetTickCount64();
        if (now < next_check_) return active_;
        next_check_ = now + 250;
        uint64_t until = 0;
        DWORD size = sizeof(until);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", L"MaintenanceUntil",
                     RRF_RT_REG_QWORD, nullptr, &until, &size);
        FILETIME time;
        GetSystemTimeAsFileTime(&time);
        const uint64_t current = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        active_ = until > current && until - current <= 120ull * 10000000;
        return active_;
    }
  private:
    uint64_t next_check_{};
    bool active_{};
};
inline bool has_hid_usage(HANDLE handle, USHORT page, USHORT usage) {
    PHIDP_PREPARSED_DATA data = nullptr;
    if (!HidD_GetPreparsedData(handle, &data))
        return false;
    HIDP_CAPS caps{};
    const auto result = HidP_GetCaps(data, &caps);
    HidD_FreePreparsedData(data);
    return result == HIDP_STATUS_SUCCESS && caps.UsagePage == page && caps.Usage == usage;
}
inline Handle open_hid_collection(USHORT page, USHORT usage, const std::wstring &serial,
                                  DWORD access = GENERIC_READ | GENERIC_WRITE) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    for (const auto &path : device_paths(guid)) {
        Handle handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        WCHAR value[64]{};
        if (handle.valid() && has_hid_usage(handle.get(), page, usage) &&
            HidD_GetSerialNumberString(handle.get(), value, sizeof(value)) && serial == value)
            return handle;
    }
    return Handle{};
}
inline bool read_hid_packet(HANDLE handle, HANDLE event, HANDLE stop, void *packet, DWORD size) {
    ResetEvent(event);
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD received = 0;
    if (!ReadFile(handle, packet, size, &received, &operation)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        const HANDLE events[]{stop, event};
        if (WaitForMultipleObjects(2, events, FALSE, 2000) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(handle, &operation);
            GetOverlappedResult(handle, &operation, &received, TRUE);
            return false;
        }
        if (!GetOverlappedResult(handle, &operation, &received, FALSE)) return false;
    }
    return received == size;
}
} // namespace switch2
