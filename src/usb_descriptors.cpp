#include "usb_descriptors.hpp"
#include "windows_support.hpp"
#include <winioctl.h>
#include <usbioctl.h>
#include "usb.hpp"

namespace switch2 {
void describe_usb_configuration(std::span<const uint8_t> bytes, std::ostream &output) {
    for (size_t offset = 0; offset + 2 <= bytes.size();) {
        const auto size = bytes[offset];
        if (size < 2 || offset + size > bytes.size())
            throw std::runtime_error("Invalid USB configuration descriptor");
        const auto type = bytes[offset + 1];
        if (type == USB_INTERFACE_DESCRIPTOR_TYPE && size >= 9)
            output << "usb_interface=" << unsigned(bytes[offset + 2])
                   << " alternate=" << unsigned(bytes[offset + 3])
                   << " class=" << unsigned(bytes[offset + 5]) << '\n';
        if (type == USB_ENDPOINT_DESCRIPTOR_TYPE && size >= 7)
            output << "usb_endpoint=" << unsigned(bytes[offset + 2])
                   << " attributes=" << unsigned(bytes[offset + 3])
                   << " max_packet=" << unsigned(bytes[offset + 4] | (bytes[offset + 5] << 8))
                   << " bInterval=" << unsigned(bytes[offset + 6]) << '\n';
        offset += size;
    }
}

void describe_controller_usb_port(std::ostream &output) {
    // Composite WinUSB filters GET_DESCRIPTOR down to its assigned interfaces.
    // Ask the parent hub for the device's complete configuration, as USBView does.
    constexpr GUID hub_guid{
        0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};
    for (const auto &path : device_paths(hub_guid)) {
        Handle hub(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr));
        if (!hub.valid())
            continue;
        USB_NODE_INFORMATION node{};
        DWORD received = 0;
        if (!DeviceIoControl(hub.get(), IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), &node,
                             sizeof(node), &received, nullptr))
            continue;
        for (ULONG port = 1; port <= node.u.HubInformation.HubDescriptor.bNumberOfPorts; ++port) {
            std::vector<uint8_t> data(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) +
                                      32 * sizeof(USB_PIPE_INFO));
            auto info = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX *>(data.data());
            info->ConnectionIndex = port;
            if (!DeviceIoControl(hub.get(), IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                                 data.data(), ULONG(data.size()), data.data(), ULONG(data.size()),
                                 &received, nullptr) ||
                info->ConnectionStatus != DeviceConnected ||
                info->DeviceDescriptor.idVendor != 0x057e ||
                info->DeviceDescriptor.idProduct != 0x2069)
                continue;
            output << "physical_usb_port=" << port << " usb_device_speed=" << unsigned(info->Speed)
                   << " configurations=" << unsigned(info->DeviceDescriptor.bNumConfigurations)
                   << '\n';
            const auto configurations = info->DeviceDescriptor.bNumConfigurations;
            for (uint8_t index = 0; index < configurations; ++index) {
                std::vector<uint8_t> descriptor(sizeof(USB_DESCRIPTOR_REQUEST) + 4096);
                auto request = reinterpret_cast<USB_DESCRIPTOR_REQUEST *>(descriptor.data());
                request->ConnectionIndex = port;
                request->SetupPacket.wValue =
                    USHORT((USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | index);
                request->SetupPacket.wLength = 4096;
                if (!DeviceIoControl(hub.get(), IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                                     descriptor.data(), ULONG(descriptor.size()), descriptor.data(),
                                     ULONG(descriptor.size()), &received, nullptr))
                    fail("Read physical USB configuration descriptor");
                if (received < sizeof(USB_DESCRIPTOR_REQUEST))
                    throw std::runtime_error("Short physical USB configuration descriptor");
                output << "physical_configuration_index=" << unsigned(index)
                       << " bytes=" << received - sizeof(USB_DESCRIPTOR_REQUEST) << '\n';
                describe_usb_configuration(std::span(descriptor)
                                               .subspan(sizeof(USB_DESCRIPTOR_REQUEST),
                                                        received - sizeof(USB_DESCRIPTOR_REQUEST)),
                                           output);
            }
            return;
        }
    }
    throw std::runtime_error("Controller hub port unavailable; elevation may be required");
}
} // namespace switch2
