#pragma once
#include "windows_support.hpp"
#include "protocol.hpp"
#include <winusb.h>

namespace switch2 {
struct HidInfo {
    std::wstring path;
    uint16_t input_length{}, output_length{}, feature_length{}, usage_page{}, usage{};
};
std::vector<HidInfo> inspect_usb();
class UsbController {
  public:
    explicit UsbController(const HidInfo &info);
    ~UsbController();
    UsbController(const UsbController &) = delete;
    UsbController &operator=(const UsbController &) = delete;
    void initialize();
    Calibration calibration();
    std::vector<uint8_t> read_memory(uint32_t address, uint8_t length);
    std::vector<uint8_t> read(unsigned timeout_ms);
    void write(std::span<const uint8_t> report);

  private:
    Handle hid_, read_event_, write_event_;
    uint16_t input_length_, output_length_;
};
} // namespace switch2
