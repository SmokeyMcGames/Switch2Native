#include "joy_ipc.hpp"
#include "settings_backend.hpp"
#include "hid_helpers.hpp"
#include "windows_pairing.hpp"
#include "winrt_wait.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <cfgmgr32.h>
#include <sddl.h>
#include <winrt/base.h>
#include <mutex>
#include <thread>
#include <wtsapi32.h>
#include <userenv.h>
#include <filesystem>
#include <type_traits>

using namespace switch2;
namespace ipc = switch2::joy_ipc;
namespace {
SERVICE_STATUS_HANDLE service_handle{};
Handle stop_event;
std::mutex state_mutex;
ipc::Response state;
std::optional<JoyRequest> pending_choice;
std::optional<JoyRequest> pending_mouse_mode;

// The service owns only the launch handle, never a desktop window. Run the
// installed tray binary with the console user's token, not LocalSystem's.
class TrayLauncher {
  public:
    void update(bool connected) {
        const auto session = WTSGetActiveConsoleSessionId();
        if (session != session_) {
            process_.reset();
            session_ = session;
            dismissed_ = false;
        }
        if (!connected) dismissed_ = false;
        if (process_.valid()) {
            if (WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) return;
            DWORD code = 0;
            if (GetExitCodeProcess(process_.get(), &code) && code == 2 && connected)
                dismissed_ = true;
            process_.reset();
        }
        const auto now = GetTickCount64();
        if (!connected || dismissed_ || session == 0xffffffff || now < next_attempt_) return;
        next_attempt_ = now + 5000;
        HANDLE raw_token{};
        if (!WTSQueryUserToken(session, &raw_token)) return;
        Handle token(raw_token);
        wchar_t module[32768]{};
        const auto length = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
        if (!length || length >= ARRAYSIZE(module)) return;
        const auto executable = (std::filesystem::path(module).parent_path() / L"Switch2JoyCons.exe").wstring();
        auto command = L"\"" + executable + L"\" --connected-only";
        void *environment{};
        if (!CreateEnvironmentBlock(&environment, token.get(), FALSE)) return;
        STARTUPINFOW startup{sizeof(startup)};
        wchar_t desktop[] = L"winsta0\\default";
        startup.lpDesktop = desktop;
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        const bool launched = CreateProcessAsUserW(token.get(), executable.c_str(), command.data(),
            nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, environment,
            std::filesystem::path(executable).parent_path().c_str(), &startup, &child) != FALSE;
        DestroyEnvironmentBlock(environment);
        if (launched) {
            CloseHandle(child.hThread);
            process_.reset(child.hProcess);
        }
    }
  private:
    Handle process_;
    DWORD session_{0xffffffff};
    uint64_t next_attempt_{};
    bool dismissed_{};
};

void set_pairing_mask(DWORD mask) {
    HKEY key{};
    const auto opened =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", 0, KEY_SET_VALUE, &key);
    if (opened != ERROR_SUCCESS)
        throw std::runtime_error("Cannot pause Joy-Con connection for pairing.");
    const auto result = RegSetValueExW(key, L"JoyPairingMask", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&mask), sizeof(mask));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("Cannot pause Joy-Con connection for pairing.");
}
struct StopOnExit {
    ~StopOnExit() {
        SetEvent(stop_event.get());
    }
};

void report_service(DWORD current, DWORD error = NO_ERROR) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = current;
    status.dwWin32ExitCode = error;
    status.dwControlsAccepted =
        current == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWaitHint = current == SERVICE_STOP_PENDING ? 15000 : 0;
    if (service_handle)
        SetServiceStatus(service_handle, &status);
}
bool stopping() {
    return WaitForSingleObject(stop_event.get(), 0) != WAIT_TIMEOUT;
}
void WINAPI control(DWORD command) {
    if (command == SERVICE_CONTROL_STOP || command == SERVICE_CONTROL_SHUTDOWN) {
        report_service(SERVICE_STOP_PENDING);
        SetEvent(stop_event.get());
    }
}

// Only recorded Switch2Native virtual nodes are eligible for changes.
// Recheck their exact hardware ID before every enable/disable operation.
CONFIGRET set_node(const std::wstring &role, bool enabled) {
    wchar_t instance[MAX_DEVICE_ID_LEN]{};
    const auto value_name = role == L"Native" ? std::wstring(L"DeviceId") : role + L"DeviceId";
    DWORD bytes = sizeof(instance);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", value_name.c_str(),
                     RRF_RT_REG_SZ, nullptr, instance, &bytes) != ERROR_SUCCESS)
        return CR_NO_SUCH_DEVNODE;
    if (wcsncmp(instance, L"ROOT\\", 5) != 0)
        return CR_INVALID_DEVNODE;
    DEVINST device{};
    auto result = CM_Locate_DevNodeW(&device, instance, CM_LOCATE_DEVNODE_NORMAL);
    if (result != CR_SUCCESS)
        return result;
    wchar_t hardware[512]{};
    ULONG type = 0, length = sizeof(hardware);
    result =
        CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_HARDWAREID, &type, hardware, &length, 0);
    const auto expected = L"ROOT\\SWITCH2" + role;
    if (result != CR_SUCCESS || type != REG_MULTI_SZ || _wcsicmp(hardware, expected.c_str()) != 0)
        return CR_INVALID_DEVNODE;
    ULONG flags = 0, problem = 0;
    result = CM_Get_DevNode_Status(&flags, &problem, device, 0);
    if (result != CR_SUCCESS)
        return result;
    if (enabled && !problem && (flags & DN_STARTED))
        return CR_SUCCESS;
    if (!enabled && problem == CM_PROB_DISABLED)
        return CR_SUCCESS;
    return enabled ? CM_Enable_DevNode(device, 0)
                   : CM_Disable_DevNode(device, CM_DISABLE_UI_NOT_OK | CM_DISABLE_PERSIST);
}
CONFIGRET set_gamepad(const std::wstring &gamepad, const std::wstring &motion, bool enabled) {
    auto result = enabled ? set_node(gamepad, true) : set_node(motion, false);
    if (result == CR_SUCCESS)
        result = enabled ? set_node(motion, true) : set_node(gamepad, false);
    return result;
}
CONFIGRET set_joy_gamepad(unsigned slot, bool enabled) {
    const auto suffix = std::to_wstring(slot + 1);
    const auto sensor = L"JoySensor" + suffix;
    auto result = enabled ? CR_SUCCESS : set_node(sensor, false);
    if (result == CR_SUCCESS)
        result = set_gamepad(L"JoyPad" + suffix, L"JoyMotion" + suffix, enabled);
    if (result == CR_SUCCESS && enabled) result = set_node(sensor, true);
    return result;
}
CONFIGRET set_pro_gamepad(bool enabled) {
    auto result = enabled ? CR_SUCCESS : set_node(L"ProSensor", false);
    if (result == CR_SUCCESS) result = set_gamepad(L"Native", L"Motion", enabled);
    if (result == CR_SUCCESS && enabled) result = set_node(L"ProSensor", true);
    return result;
}
void synchronize_nodes(uint8_t desired) {
    uint32_t actual = 0, error = 0;
    const bool pro_enabled = desired != 0x0f;
    // Release gamepad slots before enabling replacements. The physical Pro hub
    // stays alive while four separate Joy-Cons occupy all four XInput slots.
    if (!pro_enabled)
        error = set_pro_gamepad(false);
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
        if (!(desired & (1u << slot))) {
            const auto result = set_joy_gamepad(slot, false);
            if (result != CR_SUCCESS)
                error = result;
        }
    if (pro_enabled) {
        const auto result = set_pro_gamepad(true);
        if (result != CR_SUCCESS)
            error = result;
    }
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
        if (desired & (1u << slot)) {
            const auto result = set_joy_gamepad(slot, true);
            if (result == CR_SUCCESS)
                actual |= 1u << slot;
            else
                error = result;
        }
    std::lock_guard lock(state_mutex);
    state.enabled_slots = actual;
    state.device_error = error;
}
void synchronize_windows_pairings(const std::vector<WindowsPairedController> &paired) {
    HKEY raw{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &raw) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot open controller configuration");
    using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;
    Key key(raw, RegCloseKey);
    auto read_address = [&](const std::wstring &name) {
        uint64_t value = 0;
        DWORD bytes = sizeof(value);
        RegGetValueW(key.get(), nullptr, name.c_str(), RRF_RT_REG_QWORD, nullptr, &value, &bytes);
        return value;
    };
    auto write_address = [&](const std::wstring &name, uint64_t value) {
        uint64_t current = 0;
        DWORD bytes = sizeof(current);
        if (RegGetValueW(key.get(), nullptr, name.c_str(), RRF_RT_REG_QWORD, nullptr,
                         &current, &bytes) == ERROR_SUCCESS && current == value) return;
        if (RegSetValueExW(key.get(), name.c_str(), 0, REG_QWORD,
                          reinterpret_cast<const BYTE *>(&value), sizeof(value)) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot save Windows controller association");
    };
    auto find = [&](uint64_t address) {
        return std::find_if(paired.begin(), paired.end(),
                            [&](const auto &device) { return device.address == address; });
    };
    auto pro = read_address(L"BluetoothAddress");
    const auto current_pro = find(pro);
    if (current_pro == paired.end() || current_pro->kind != ControllerKind::Pro) pro = 0;
    if (!pro) {
        const auto next = std::find_if(paired.begin(), paired.end(),
                                     [](const auto &device) { return device.kind == ControllerKind::Pro; });
        if (next != paired.end()) pro = next->address;
    }
    write_address(L"BluetoothAddress", pro);
    std::array<uint64_t, MaxJoyCons> addresses{};
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot) {
        addresses[slot] = read_address(L"JoyAddress" + std::to_wstring(slot + 1));
        const auto device = find(addresses[slot]);
        if (device == paired.end() || device->kind == ControllerKind::Pro) addresses[slot] = 0;
    }
    for (const auto &device : paired) {
        if (device.kind == ControllerKind::Pro ||
            std::find(addresses.begin(), addresses.end(), device.address) != addresses.end()) continue;
        const auto available = std::find(addresses.begin(), addresses.end(), 0);
        if (available != addresses.end()) *available = device.address;
    }
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot) {
        const auto suffix = std::to_wstring(slot + 1);
        const auto device = find(addresses[slot]);
        if (device != paired.end()) {
            const DWORD product = uint16_t(device->kind);
            DWORD current = 0, bytes = sizeof(current);
            const auto name = L"JoyKind" + suffix;
            RegGetValueW(key.get(), nullptr, name.c_str(), RRF_RT_REG_DWORD, nullptr, &current, &bytes);
            if (current != product && RegSetValueExW(key.get(), name.c_str(), 0, REG_DWORD,
                              reinterpret_cast<const BYTE *>(&product), sizeof(product)) != ERROR_SUCCESS)
                throw std::runtime_error("Cannot save Windows controller type");
        }
        // Explicit zero entries suppress legacy left/right address fallbacks.
        write_address(L"JoyAddress" + suffix, addresses[slot]);
    }
}
// This holder is created only on a new Windows-pairing transition. It bridges
// Windows' short initial link to the coordinator, without claiming open SYNC
// advertisements or renewing old saved pairings when the service starts.
struct WindowsPairingHandoff {
    using BluetoothLEDevice = winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
    using GattSession = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession;
    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    uint64_t address{}, expires{};
    template <class Operation> auto await(Operation operation) {
        return wait_winrt(operation, [] {
            if (stopping()) throw std::runtime_error("Windows pairing handoff stopped");
        }, 3000);
    }
    explicit WindowsPairingHandoff(uint64_t target) : address(target), expires(GetTickCount64() + 10000) {
        try {
            device = await(BluetoothLEDevice::FromBluetoothAddressAsync(address));
            if (!device || !device.DeviceInformation().Pairing().IsPaired())
                throw std::runtime_error("Windows pairing was removed");
            session = await(GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()));
            if (!session) throw std::runtime_error("Windows pairing session unavailable");
            session.MaintainConnection(true);
            if (device.ConnectionStatus() != winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus::Connected) {
                const winrt::guid service{L"ab7de9be-89fe-49ad-828f-118f09df7fd0"};
                await(device.GetGattServicesForUuidAsync(service,
                    winrt::Windows::Devices::Bluetooth::BluetoothCacheMode::Uncached));
            }
        } catch (...) { close(); throw; }
    }
    void close() noexcept {
        try { if (session) { session.MaintainConnection(false); session.Close(); } } catch (...) {}
        try { if (device) device.Close(); } catch (...) {}
    }
    ~WindowsPairingHandoff() { close(); }
};
bool controller_has_input(uint64_t address) {
    uint8_t joy_connected;
    bool pro_connected;
    {
        std::lock_guard lock(state_mutex);
        joy_connected = state.status.connected;
        pro_connected = state.pro_connected != 0;
    }
    auto matches = [&](const std::wstring &name) {
        uint64_t configured = 0;
        DWORD bytes = sizeof(configured);
        return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", name.c_str(),
                            RRF_RT_REG_QWORD, nullptr, &configured, &bytes) == ERROR_SUCCESS &&
               configured == address;
    };
    if (pro_connected && matches(L"BluetoothAddress")) return true;
    for (unsigned slot = 0; slot < MaxJoyCons; ++slot)
        if ((joy_connected & (1u << slot)) && matches(L"JoyAddress" + std::to_wstring(slot + 1))) return true;
    return false;
}
void watch_windows_pairings(std::stop_token stop) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (...) { return; }
    try {
        using namespace winrt::Windows::Devices::Enumeration;
        using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
        // Establish a baseline before listening: an existing saved controller
        // is not permission to start a fresh pairing handoff.
        auto previous = windows_paired_controllers();
        auto changed = std::make_shared<Handle>(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!changed->valid()) fail("Create Windows pairing event");
        auto watcher = DeviceInformation::CreateWatcher(
            BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), nullptr,
            DeviceInformationKind::AssociationEndpoint);
        auto added = watcher.Added(winrt::auto_revoke, [changed](auto const &, auto const &) { SetEvent(changed->get()); });
        auto updated = watcher.Updated(winrt::auto_revoke, [changed](auto const &, auto const &) { SetEvent(changed->get()); });
        auto removed = watcher.Removed(winrt::auto_revoke, [changed](auto const &, auto const &) { SetEvent(changed->get()); });
        std::vector<std::unique_ptr<WindowsPairingHandoff>> handoffs;
        watcher.Start();
        while (!stop.stop_requested() && !stopping()) {
            try {
                auto current = windows_paired_controllers();
                for (const auto &controller : current) {
                    if (std::none_of(previous.begin(), previous.end(), [&](const auto &old) { return old.address == controller.address; })) {
                        try { handoffs.push_back(std::make_unique<WindowsPairingHandoff>(controller.address)); }
                        catch (...) { OutputDebugStringW(L"Switch2Native: initial Windows pairing handoff failed\n"); }
                    }
                }
                synchronize_windows_pairings(current);
                previous = std::move(current);
                std::erase_if(handoffs, [&](const auto &handoff) {
                    return controller_has_input(handoff->address) || GetTickCount64() >= handoff->expires ||
                           std::none_of(previous.begin(), previous.end(), [&](const auto &device) { return device.address == handoff->address; });
                });
            } catch (...) { OutputDebugStringW(L"Switch2Native: Windows pairing enumeration failed; retaining prior configuration\n"); }
            const HANDLE events[]{stop_event.get(), changed->get()};
            WaitForMultipleObjects(2, events, FALSE, handoffs.empty() ? 1000 : 200);
        }
        watcher.Stop();
    } catch (...) {
        OutputDebugStringW(L"Switch2Native: Windows pairing watcher unavailable\n");
    }
    winrt::uninit_apartment();
}
ipc::Response process_request(const ipc::Request &request, bool interactive) {
    std::unique_lock lock(state_mutex);
    DWORD result = ERROR_SUCCESS;
    if (request.magic != 0x53324a43 || request.version != 3)
        result = ERROR_INVALID_DATA;
    else if (request.command == ipc::Command::Status) {
    } else if (!interactive)
        result = ERROR_ACCESS_DENIED;
    else if (request.command == ipc::Command::Choose) {
        if (!state.available ||
            (!request.pair ||
             (request.pair != state.status.pairs[0] && request.pair != state.status.pairs[1])) ||
            request.generation != state.status.generation ||
            (request.mode != uint32_t(JoyMode::Combined) &&
             request.mode != uint32_t(JoyMode::Separate)))
            result = ERROR_INVALID_STATE;
        else {
            JoyRequest choice;
            choice.generation = request.generation;
            choice.value = uint8_t(request.mode);
            choice.pair = uint8_t(request.pair);
            pending_choice = choice;
        }
    } else if (request.command == ipc::Command::MouseMode || request.command == ipc::Command::MouseSource) {
        if (!state.available || request.mode > 1 || (request.mode && !state.status.connected))
            result = ERROR_INVALID_STATE;
        else {
            JoyRequest mode;
            mode.command = request.command == ipc::Command::MouseMode ? JoyCommand::MouseMode : JoyCommand::MouseSource;
            mode.value = uint8_t(request.mode);
            pending_mouse_mode = mode;
        }
    } else if (request.command == ipc::Command::PairLeft ||
               request.command == ipc::Command::PairRight) {
        result = ERROR_NOT_SUPPORTED;
        wcscpy_s(state.message, L"Pair controllers in Windows Settings > Bluetooth & devices.");
    } else
        result = ERROR_INVALID_FUNCTION;
    auto response = state;
    response.result = result;
    return response;
}
void serve_pipe(HANDLE pipe) {
    SettingsBackend settings;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid())
        return;
    while (!stopping()) {
        ResetEvent(event.get());
        OVERLAPPED connection{};
        connection.hEvent = event.get();
        bool connected = ConnectNamedPipe(pipe, &connection) != FALSE;
        if (!connected) {
            const auto error = GetLastError();
            connected = error == ERROR_PIPE_CONNECTED;
            if (error == ERROR_IO_PENDING) {
                const HANDLE waits[]{stop_event.get(), event.get()};
                DWORD transferred = 0;
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                    connected =
                        GetOverlappedResult(pipe, &connection, &transferred, FALSE) != FALSE;
                else {
                    CancelIoEx(pipe, &connection);
                    GetOverlappedResult(pipe, &connection, &transferred, TRUE);
                }
            }
        }
        if (connected && !stopping()) {
            ipc::Request request;
            if (ipc::transfer(pipe, event.get(), stop_event.get(), &request, sizeof(request),
                              false)) {
                ULONG process = 0;
                DWORD session = 0;
                const bool interactive = GetNamedPipeClientProcessId(pipe, &process) &&
                                         ProcessIdToSessionId(process, &session) &&
                                         session == WTSGetActiveConsoleSessionId();
                if (request.command == ipc::Command::SettingsRead || request.command == ipc::Command::SettingsSave) {
                    auto response = settings.process(request, interactive);
                    ipc::transfer(pipe, event.get(), stop_event.get(), &response, sizeof(response), true);
                } else {
                    auto response = process_request(request, interactive);
                    ipc::transfer(pipe, event.get(), stop_event.get(), &response, sizeof(response), true);
                }
                // The client closes after consuming one response. A bounded read
                // prevents DisconnectNamedPipe from discarding an unread response.
                uint8_t unexpected{};
                ipc::transfer(pipe, event.get(), stop_event.get(), &unexpected, 1, false, 500);
            }
        }
        DisconnectNamedPipe(pipe);
    }
}
void run() {
    set_pairing_mask(0);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &descriptor, nullptr))
        fail("Create pipe security");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle pipe(CreateNamedPipeW(
        ipc::PipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        sizeof(ipc::SettingsResponse), sizeof(ipc::Request), 0, &security));
    LocalFree(descriptor);
    if (!pipe.valid())
        fail("Create Joy-Con manager pipe");
    std::jthread server([&] { serve_pipe(pipe.get()); });
    StopOnExit stop_on_exit;
    std::jthread windows_pairings(watch_windows_pairings);
    Handle hub, pro_hub;
    TrayLauncher tray;
    uint64_t last_sync = 0;
    uint8_t previous = 255;
    report_service(SERVICE_RUNNING);
    while (!stopping()) {
        if (!hub.valid()) {
            try {
                hub = open_hid_collection(0xff00, 3, L"Switch2Native-JoyHub", 0);
            } catch (...) {
            }
        }
        JoyStatus status;
        const bool available = hub.valid() && HidD_GetFeature(hub.get(), &status, sizeof(status)) &&
                               status.version == 2;
        if (!pro_hub.valid()) {
            try { pro_hub = open_hid_collection(0xff00, 3, L"Switch2Native-ProHub", 0); }
            catch (...) {}
        }
        JoyStatus pro;
        const bool pro_available = pro_hub.valid() && HidD_GetFeature(pro_hub.get(), &pro, sizeof(pro)) && pro.version == 2;
        if (!pro_available) pro_hub.reset();
        std::optional<JoyRequest> choice;
        std::optional<JoyRequest> mouse_mode;
        {
            std::lock_guard lock(state_mutex);
            state.available = available;
            state.status = available ? status : JoyStatus{};
            state.pro_connected = pro_available && pro.connected;
            choice = std::exchange(pending_choice, {});
            mouse_mode = std::exchange(pending_mouse_mode, {});
        }
        if (available && choice) {
            if (!HidD_SetFeature(hub.get(), &*choice, sizeof(*choice))) {
                std::lock_guard lock(state_mutex);
                wcscpy_s(state.message,
                         L"Connection changed before the choice was applied. Choose again.");
            }
        }
        if (!available)
            hub.reset();
        if (available && mouse_mode && !HidD_SetFeature(hub.get(), &*mouse_mode, sizeof(*mouse_mode))) {
            std::lock_guard lock(state_mutex);
            wcscpy_s(state.message, L"Mouse mode could not be changed. Try again.");
        }
        tray.update((available && status.connected) || (pro_available && pro.connected));
        const auto desired = available ? status.active_slots : uint8_t{0};
        if (desired != previous || GetTickCount64() - last_sync >= 2000) {
            synchronize_nodes(desired);
            last_sync = GetTickCount64();
            previous = desired;
        }
        WaitForSingleObject(stop_event.get(), 200);
    }
    server.join();
    windows_pairings.join();
    hub.reset();
    pro_hub.reset();
    // Service maintenance does not mean physical controllers disconnected.
    // Leave the XInput nodes in place; their readers deliver neutral input when
    // a hub closes. The updater controls any necessary device restart itself.
}
void WINAPI service_main(DWORD, LPWSTR *) {
    service_handle = RegisterServiceCtrlHandlerW(ipc::ServiceName, control);
    if (!service_handle)
        return;
    report_service(SERVICE_START_PENDING);
    try {
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", L"MaintenanceUntil");
        stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_event.valid())
            fail("Create manager stop event");
        run();
        report_service(SERVICE_STOPPED);
    } catch (...) {
        if (stop_event.valid())
            SetEvent(stop_event.get());
        report_service(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }
}
} // namespace
int wmain() {
    SERVICE_TABLE_ENTRYW table[]{{const_cast<wchar_t *>(ipc::ServiceName), service_main},
                                 {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : int(GetLastError());
}
