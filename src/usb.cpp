#include "usb.hpp"
#include <setupapi.h>
#include <hidsdi.h>
#include <array>
#include <memory>

namespace switch2 {
std::vector<std::wstring> device_paths(const GUID &guid) {
    HDEVINFO devices =
        SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
        fail("SetupDiGetClassDevs");
    auto cleanup = std::unique_ptr<void, decltype(&SetupDiDestroyDeviceInfoList)>(
        devices, SetupDiDestroyDeviceInfoList);
    std::vector<std::wstring> paths;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{sizeof(SP_DEVICE_INTERFACE_DATA)};
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            fail("SetupDiEnumDeviceInterfaces");
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &size, nullptr);
        if (!size)
            fail("SetupDiGetDeviceInterfaceDetail size");
        std::vector<uint8_t> buffer(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, size, nullptr,
                                              nullptr))
            fail("SetupDiGetDeviceInterfaceDetail");
        paths.emplace_back(detail->DevicePath);
    }
    return paths;
}
std::vector<HidInfo> inspect_usb() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    std::vector<HidInfo> devices;
    for (const auto &path : device_paths(guid)) {
        if (!is_controller(path))
            continue;
        Handle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr));
        if (!handle.valid())
            fail("Open HID metadata");
        PHIDP_PREPARSED_DATA data = nullptr;
        if (!HidD_GetPreparsedData(handle.get(), &data))
            fail("HidD_GetPreparsedData");
        HIDP_CAPS caps{};
        const auto result = HidP_GetCaps(data, &caps);
        HidD_FreePreparsedData(data);
        if (result != HIDP_STATUS_SUCCESS)
            throw std::runtime_error("Cannot decode HID capabilities");
        devices.push_back({path, caps.InputReportByteLength, caps.OutputReportByteLength,
                           caps.FeatureReportByteLength, caps.UsagePage, caps.Usage});
    }
    return devices;
}
UsbController::UsbController(const HidInfo &info)
    : input_length_(info.input_length), output_length_(info.output_length) {
    if (!input_length_ || input_length_ > 1024)
        throw std::runtime_error("Unexpected HID report size");
    hid_.reset(CreateFileW(info.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr));
    if (!hid_.valid())
        fail("Open controller HID");
    read_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    write_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!read_event_.valid() || !write_event_.valid())
        fail("Create controller I/O events");
    HidD_SetNumInputBuffers(hid_.get(), 64);
}
UsbController::~UsbController() = default;
namespace {
class VendorInterface {
  public:
    VendorInterface() {
        const GUID vendor{
            0x6f13725e, 0xef0e, 0x4fd3, {0xae, 0x5f, 0xb2, 0xde, 0x98, 0x9e, 0xc8, 0x25}};
        for (const auto &path : device_paths(vendor)) {
            if (!is_controller(path))
                continue;
            file_.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr));
            if (!file_.valid())
                fail("Open controller WinUSB");
            WINUSB_INTERFACE_HANDLE usb = nullptr;
            if (!WinUsb_Initialize(file_.get(), &usb))
                fail("WinUsb_Initialize");
            usb_.reset(usb);
            USB_INTERFACE_DESCRIPTOR descriptor{};
            if (!WinUsb_QueryInterfaceSettings(usb, 0, &descriptor))
                fail("WinUsb_QueryInterfaceSettings");
            for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
                WINUSB_PIPE_INFORMATION pipe{};
                if (!WinUsb_QueryPipe(usb, 0, index, &pipe))
                    fail("WinUsb_QueryPipe");
                if (pipe.PipeType == UsbdPipeTypeBulk) {
                    if (pipe.PipeId & 0x80)
                        input_ = pipe.PipeId;
                    else
                        output_ = pipe.PipeId;
                }
            }
            if (!output_ || !input_)
                throw std::runtime_error("Controller vendor bulk endpoints missing");
            ULONG timeout = 1000;
            for (auto pipe : {input_, output_})
                if (!WinUsb_SetPipePolicy(usb, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout),
                                          &timeout))
                    fail("Set USB timeout");
            WinUsb_FlushPipe(usb, input_);
            return;
        }
        throw std::runtime_error("Controller vendor interface not found");
    }
    std::vector<uint8_t> exchange(std::vector<UCHAR> command, size_t expected = 64) {
        ULONG written = 0;
        if (!WinUsb_WritePipe(usb_.get(), output_, command.data(),
                              static_cast<ULONG>(command.size()), &written, nullptr))
            fail("Write vendor command");
        if (written != command.size())
            throw std::runtime_error("Short USB vendor command");
        std::vector<uint8_t> response(expected);
        size_t received = 0;
        while (received < expected) {
            ULONG part = 0;
            const auto chunk = static_cast<ULONG>(std::min(size_t(64), expected - received));
            if (!WinUsb_ReadPipe(usb_.get(), input_, response.data() + received, chunk, &part,
                                 nullptr)) {
                const auto error = GetLastError();
                throw std::system_error(int(error), std::system_category(),
                    "USB reply " + std::to_string(command[0]) + "/" + std::to_string(command[3]));
            }
            received += part;
            if (part < chunk)
                break;
        }
        response.resize(received);
        if (response.size() < 8 || response[0] != command[0] || response[1] != 1)
            throw std::runtime_error("Unexpected USB vendor response");
        return response;
    }
    std::vector<uint8_t> memory(uint32_t address) {
        auto response =
            exchange({2, 0x91, 0, 1, 0, 8, 0, 0, 0, 0, 0, 0, uint8_t(address),
                      uint8_t(address >> 8), uint8_t(address >> 16), uint8_t(address >> 24)},
                     80);
        if (response.size() != 80)
            throw std::runtime_error("Incomplete USB calibration response");
        return {response.begin() + 16, response.end()};
    }

  private:
    Handle file_;
    std::unique_ptr<void, decltype(&WinUsb_Free)> usb_{nullptr, WinUsb_Free};
    UCHAR input_{}, output_{};
};
} // namespace
void UsbController::initialize() {
    VendorInterface vendor;
    const std::vector<std::vector<UCHAR>> commands{
        // Select wired operation before the generic initialization exchange.
        // During a Bluetooth handoff that exchange may otherwise never reply.
        {0x03, 0x91, 0, 0x0d, 0, 8, 0, 0, 1, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {0x07, 0x91, 0, 1, 0, 0, 0, 0},
        {0x0c, 0x91, 0, 2, 0, 4, 0, 0, 0x27, 0, 0, 0},
        {0x0c, 0x91, 0, 4, 0, 4, 0, 0, 0x27, 0, 0, 0},
        {0x03, 0x91, 0, 0x0a, 0, 4, 0, 0, 5, 0, 0, 0},
        {0x03, 0x91, 0, 0x0d, 0, 8, 0, 0, 1, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {9, 0x91, 0, 7, 0, 8, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}};
    for (auto command : commands)
        vendor.exchange(std::move(command));
}
Calibration UsbController::calibration() {
    VendorInterface vendor;
    Calibration calibration;
    for (size_t side = 0; side < 2; ++side) {
        auto factory = vendor.memory(uint32_t(0x13080 + side * 64));
        if (!parse_calibration(std::span(factory).subspan(0x28), calibration, side * 2))
            throw std::runtime_error("Invalid factory stick calibration");
        auto user = vendor.memory(uint32_t(0x1fc040 + side * 64));
        if (user[0] == 0xb2 && user[1] == 0xa1 &&
            !parse_calibration(std::span(user).subspan(2), calibration, side * 2))
            throw std::runtime_error("Invalid user stick calibration");
    }
    return calibration;
}
std::vector<uint8_t> UsbController::read_memory(uint32_t address, uint8_t length) {
    if ((address & 63) || !length || length > 64)
        throw std::runtime_error("Invalid USB flash block read");
    VendorInterface vendor;
    auto data = vendor.memory(address);
    data.resize(length);
    return data;
}
std::vector<uint8_t> UsbController::read(unsigned timeout_ms) {
    std::vector<uint8_t> buffer(input_length_);
    ResetEvent(read_event_.get());
    OVERLAPPED operation{};
    operation.hEvent = read_event_.get();
    DWORD received = 0;
    if (!ReadFile(hid_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &received,
                  &operation)) {
        if (GetLastError() != ERROR_IO_PENDING)
            fail("Read controller");
        const auto result = WaitForSingleObject(read_event_.get(), timeout_ms);
        if (result != WAIT_OBJECT_0) {
            CancelIoEx(hid_.get(), &operation);
            GetOverlappedResult(hid_.get(), &operation, &received, TRUE);
            if (result == WAIT_TIMEOUT)
                return {};
            throw std::runtime_error("Waiting for controller input failed");
        }
        if (!GetOverlappedResult(hid_.get(), &operation, &received, FALSE))
            fail("Complete controller read");
    }
    buffer.resize(received);
    return buffer;
}
void UsbController::write(std::span<const uint8_t> report) {
    if (report.size() > output_length_)
        throw std::runtime_error("Output report too long");
    std::vector<uint8_t> buffer(output_length_);
    std::copy(report.begin(), report.end(), buffer.begin());
    ResetEvent(write_event_.get());
    OVERLAPPED operation{};
    operation.hEvent = write_event_.get();
    DWORD written = 0;
    if (!WriteFile(hid_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &written,
                   &operation)) {
        if (GetLastError() != ERROR_IO_PENDING)
            fail("Write controller");
        if (WaitForSingleObject(write_event_.get(), 1000) != WAIT_OBJECT_0) {
            CancelIoEx(hid_.get(), &operation);
            GetOverlappedResult(hid_.get(), &operation, &written, TRUE);
            throw std::runtime_error("Controller output timed out");
        }
        if (!GetOverlappedResult(hid_.get(), &operation, &written, FALSE))
            fail("Complete controller write");
    }
    if (written != buffer.size())
        throw std::runtime_error("Short HID output transfer");
}
} // namespace switch2
