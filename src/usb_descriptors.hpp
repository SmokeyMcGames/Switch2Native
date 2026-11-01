#pragma once
#include <cstdint>
#include <iosfwd>
#include <span>

namespace switch2 {
void describe_usb_configuration(std::span<const uint8_t> bytes, std::ostream &output);
void describe_controller_usb_port(std::ostream &output);
} // namespace switch2
