#pragma once
#include "joy_protocol.hpp"
#include "windows_support.hpp"
#include "controller_settings.hpp"

namespace switch2::joy_ipc {
inline constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\Switch2Native.JoyCons.v3";
inline constexpr wchar_t ServiceName[] = L"Switch2Manager";
enum class Command : uint32_t { Status, Choose, PairLeft, PairRight, MouseMode, MouseSource, SettingsRead, SettingsSave };
struct Request {
    uint32_t magic{0x53324a43}, version{3};
    Command command{Command::Status};
    uint32_t generation{}, mode{}, pair{};
};
struct Response {
    uint32_t magic{0x53324a43}, version{3}, result{}, available{};
    JoyStatus status;
    uint32_t enabled_slots{}, device_error{}, pairing{}; // pairing: 0 idle, 1 left, 2 right
    wchar_t message[128]{};
    uint32_t pro_connected{};
};
static_assert(sizeof(Request) == 24 && sizeof(Response) == 352);
struct SettingsResponse {
    uint32_t magic{0x53324a43}, version{3}, result{}, generation{};
    std::array<ControllerSnapshot, 5> controllers{}; // Pro, then four physical Joy-Cons.
    std::array<ControllerMotion, 5> motion{};
};
static_assert(sizeof(SettingsResponse) == 656);
inline uint32_t pack_profile(const ControllerProfile &profile) {
    return profile.deadzone[0] | (uint32_t(profile.deadzone[1]) << 6) |
        (uint32_t(profile.circular_mask) << 12) | (uint32_t(profile.gyro_counts_per_degree) << 14);
}
inline ControllerProfile unpack_profile(uint32_t packed) {
    ControllerProfile profile;
    profile.deadzone[0] = uint8_t(packed & 63);
    profile.deadzone[1] = uint8_t((packed >> 6) & 63);
    profile.circular_mask = uint8_t((packed >> 12) & 3);
    profile.gyro_counts_per_degree = uint16_t((packed >> 14) & 511);
    profile.reserved = uint16_t(packed >> 23); // Reject unused bits, never silently truncate them.
    return profile;
}

// Bound every exchange so a stopped service or an abandoned client cannot hang
// the tray UI or prevent the service from shutting down.
inline bool transfer(HANDLE pipe, HANDLE event, HANDLE stop, void *data, DWORD size, bool write,
                     DWORD timeout = 1500) {
    ResetEvent(event);
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD transferred = 0;
    const BOOL done = write ? WriteFile(pipe, data, size, &transferred, &operation)
                            : ReadFile(pipe, data, size, &transferred, &operation);
    if (!done) {
        if (GetLastError() != ERROR_IO_PENDING)
            return false;
        const HANDLE events[]{event, stop};
        const DWORD result = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout);
        if (result != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            return false;
        }
        if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
            return false;
    }
    return transferred == size;
}
template<class Reply> inline bool exchange(Request request, Reply &response) {
    Handle pipe;
    const auto deadline = GetTickCount64() + 250;
    do {
        pipe.reset(CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                              nullptr));
        if (pipe.valid()) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        // Status and the settings worker share one service pipe. Availability
        // is not a reservation: retry if another client won the open race.
        WaitNamedPipeW(PipeName, 25);
    } while (GetTickCount64() < deadline);
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    DWORD mode = PIPE_READMODE_MESSAGE;
    return pipe.valid() && event.valid() &&
           SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr) &&
           transfer(pipe.get(), event.get(), nullptr, &request, sizeof(request), true, 500) &&
           transfer(pipe.get(), event.get(), nullptr, &response, sizeof(response), false, 500) &&
           response.magic == request.magic && response.version == 3;
}
} // namespace switch2::joy_ipc
