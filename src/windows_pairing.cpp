#include "windows_pairing.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>

namespace switch2 {
std::vector<WindowsPairedController> windows_paired_controllers() {
    using namespace winrt;
    using namespace Windows::Devices::Bluetooth;
    using namespace Windows::Devices::Enumeration;
    const auto properties = single_threaded_vector<hstring>({L"System.Devices.Aep.DeviceAddress"});
    auto devices = DeviceInformation::FindAllAsync(
        BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), properties,
        DeviceInformationKind::AssociationEndpoint).get();
    std::vector<WindowsPairedController> result;
    for (const auto &device : devices) {
        if (!device.Pairing().IsPaired()) continue;
        const auto name = device.Name();
        ControllerKind kind;
        if (name == L"Nintendo Switch 2 Pro Controller") kind = ControllerKind::Pro;
        else if (name == L"Joy-Con 2 (L)") kind = ControllerKind::JoyLeft;
        else if (name == L"Joy-Con 2 (R)") kind = ControllerKind::JoyRight;
        else continue;
        const auto value = device.Properties().TryLookup(L"System.Devices.Aep.DeviceAddress");
        if (!value) continue;
        const auto text = unbox_value<hstring>(value);
        uint64_t address = 0;
        unsigned digits = 0;
        for (const auto character : text) {
            if (character == L':' || character == L'-') continue;
            unsigned digit;
            if (character >= L'0' && character <= L'9') digit = character - L'0';
            else if (character >= L'a' && character <= L'f') digit = character - L'a' + 10;
            else if (character >= L'A' && character <= L'F') digit = character - L'A' + 10;
            else { digits = 0; break; }
            if (++digits > 12) break;
            address = (address << 4) | digit;
        }
        if (digits == 12 && address) result.push_back({address, kind});
    }
    return result;
}
}
