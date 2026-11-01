#include "windows_support.hpp"
#include "windows_pairing.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>

int main(int argc, char **argv) {
    try {
        using namespace winrt::Windows::Devices::Bluetooth;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (const auto &controller : switch2::windows_paired_controllers())
            std::cout << "windows_saved=" << std::hex << controller.address << " product="
                      << unsigned(controller.kind) << std::dec << '\n';
        if (argc < 2) throw std::invalid_argument("Usage: BluetoothPairingCheck ADDRESS [ADDRESS ...]");
        for (int index = 1; index < argc; ++index) {
            const auto address = std::stoull(argv[index], nullptr, 16);
            auto device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
            std::cout << "address=" << std::hex << address << std::dec;
            if (!device) { std::cout << " available=0\n"; continue; }
            auto information = device.DeviceInformation();
            auto pairing = information.Pairing();
            std::cout << " name=\"" << winrt::to_string(device.Name())
                      << "\" paired=" << pairing.IsPaired() << " can_pair=" << pairing.CanPair()
                      << " connected=" << (device.ConnectionStatus() == BluetoothConnectionStatus::Connected)
                      << "\n  id=" << winrt::to_string(information.Id()) << '\n';
            device.Close();
        }
        return 0;
    } catch (const winrt::hresult_error &error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
