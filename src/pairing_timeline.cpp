#include "windows_support.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <memory>
#include <mutex>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
namespace {
std::mutex output_mutex;
void log(const std::string &message) {
    std::lock_guard lock(output_mutex);
    std::cout << "tick_ms=" << GetTickCount64() << ' ' << message << std::endl;
}
struct Capture : std::enable_shared_from_this<Capture> {
    std::mutex mutex;
    DeviceInformation information{nullptr};
    hstring id;
    bool paired{}, enumerated{}, hold{};
    uint64_t generation{}, expires{};
    GattSession session{nullptr};
    GattSession::SessionStatusChanged_revoker session_changed;
    Windows::Foundation::IAsyncOperation<GattSession> pending{nullptr};
    void snapshot(const char *event) {
        bool start = false;
        uint64_t token;
        {
            std::lock_guard lock(mutex);
            const bool next = information && information.Pairing().IsPaired();
            std::string text = std::string(event) + " paired=" + std::to_string(next);
            if (information) {
                text += " can_pair=" + std::to_string(information.Pairing().CanPair());
                for (const auto key : {L"System.Devices.Aep.IsConnected", L"System.Devices.Aep.Bluetooth.Le.IsConnectable"}) {
                    if (auto value = information.Properties().TryLookup(key))
                        text += " " + to_string(key) + "=" + std::to_string(unbox_value<bool>(value));
                }
            }
            log(text);
            start = hold && next && !paired;
            if (next != paired) ++generation;
            paired = next;
            token = generation;
        }
        if (!start) return;
        log("fresh_pairing_session_requested");
        auto operation = GattSession::FromDeviceIdAsync(BluetoothDeviceId::FromId(id));
        {
            std::lock_guard lock(mutex);
            pending = operation;
        }
        std::weak_ptr<Capture> weak = shared_from_this();
        operation.Completed([weak, token](auto const &completed, auto status) {
            const auto state = weak.lock();
            if (!state) return;
            try {
                if (status != Windows::Foundation::AsyncStatus::Completed)
                    throw std::runtime_error("Session request did not complete");
                auto session = completed.GetResults();
                std::lock_guard lock(state->mutex);
                if (!state->paired || state->generation != token) {
                    session.Close();
                    log("session_discarded_pairing_changed");
                    return;
                }
                state->session = session;
                state->expires = GetTickCount64() + 10000;
                state->session_changed = session.SessionStatusChanged(auto_revoke, [](auto const &, auto const &event) {
                    log("session_status=" + std::to_string(int(event.Status())) + " error=" + std::to_string(int(event.Error())));
                });
                session.MaintainConnection(true);
                state->pending = nullptr;
                log("session_held status=" + std::to_string(int(session.SessionStatus())));
            } catch (const hresult_error &error) { log("session_error=" + to_string(error.message())); }
            catch (const std::exception &error) { log(std::string("session_error=") + error.what()); }
        });
    }
    void release(bool force = false) {
        GattSession closing{nullptr};
        {
            std::lock_guard lock(mutex);
            if (session && (force || !paired || GetTickCount64() >= expires)) {
                session_changed.revoke();
                closing = std::exchange(session, nullptr);
            }
        }
        if (closing) {
            try { closing.MaintainConnection(false); closing.Close(); } catch (...) {}
            log("session_released");
        }
    }
};
}
int main(int argc, char **argv) {
    try {
        if (argc < 3) throw std::invalid_argument("Usage: PairingTimeline ADDRESS SECONDS [--hold]");
        init_apartment(apartment_type::multi_threaded);
        auto state = std::make_shared<Capture>();
        state->hold = argc == 4 && std::string(argv[3]) == "--hold";
        auto device = BluetoothLEDevice::FromBluetoothAddressAsync(std::stoull(argv[1], nullptr, 16)).get();
        if (!device) throw std::runtime_error("Saved device unavailable");
        state->information = device.DeviceInformation();
        state->id = state->information.Id();
        state->paired = state->information.Pairing().IsPaired();
        device.Close();
        auto properties = single_threaded_vector<hstring>({L"System.Devices.Aep.IsConnected", L"System.Devices.Aep.IsPaired",
            L"System.Devices.Aep.DeviceAddress", L"System.Devices.Aep.Bluetooth.Le.IsConnectable"});
        const hstring all_le = L"(System.Devices.Aep.ProtocolId:=\"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}\")";
        auto watcher = DeviceInformation::CreateWatcher(all_le, properties, DeviceInformationKind::AssociationEndpoint);
        auto added = watcher.Added(auto_revoke, [state](auto const &, DeviceInformation const &information) {
            if (information.Id() != state->id) return;
            { std::lock_guard lock(state->mutex); state->information = information; }
            state->snapshot("added");
        });
        auto updated = watcher.Updated(auto_revoke, [state](auto const &, DeviceInformationUpdate const &update) {
            if (update.Id() != state->id) return;
            { std::lock_guard lock(state->mutex); if (state->information) state->information.Update(update); }
            state->snapshot("updated");
        });
        auto removed = watcher.Removed(auto_revoke, [state](auto const &, DeviceInformationUpdate const &update) {
            if (update.Id() != state->id) return;
            { std::lock_guard lock(state->mutex); state->information = nullptr; }
            state->snapshot("removed");
        });
        auto enumerated = watcher.EnumerationCompleted(auto_revoke, [state](auto const &, auto const &) {
            std::lock_guard lock(state->mutex); state->enumerated = true; log("enumeration_complete");
        });
        watcher.Start();
        log("capture_started hold=" + std::to_string(state->hold));
        const auto end = GetTickCount64() + std::stoul(argv[2]) * 1000ull;
        while (GetTickCount64() < end) { state->release(); Sleep(20); }
        watcher.Stop();
        state->release(true);
        return 0;
    } catch (const hresult_error &error) { log("error=" + to_string(error.message())); }
    catch (const std::exception &error) { log(std::string("error=") + error.what()); }
    return 1;
}
