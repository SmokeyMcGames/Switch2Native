#include "session.hpp"
#include "usb.hpp"
#include "ble.hpp"
#include "motion.hpp"
#include "packed_motion_processor.hpp"
#include <winrt/base.h>
#include <type_traits>

namespace switch2 {
namespace {
uint64_t configured_pro_address(uint64_t fallback) {
    uint64_t address = fallback;
    DWORD bytes = sizeof(address);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", L"BluetoothAddress",
                 RRF_RT_REG_QWORD, nullptr, &address, &bytes);
    return address;
}
}
Session::Session(uint64_t address, Publish publish, Log log)
    : publish_(std::move(publish)), log_(std::move(log)),
      thread_([this, address](std::stop_token stop) { run(stop, address); }) {}
Session::~Session() {
    thread_.request_stop();
    thread_.join();
}
void Session::set_rumble(uint8_t strong, uint8_t weak) noexcept {
    rumble_.store(uint16_t(strong | (unsigned(weak) << 8)));
}
template <class Controller>
void Session::pump(Controller &controller, const Calibration &calibration, Transport transport,
                   std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    auto last_input = Clock::now(), last_rumble = Clock::now();
    uint8_t sequence = 0;
    bool vibrated = false;
    std::optional<MotionProcessor> motion;
    std::optional<PackedMotionProcessor> packed;
    try {
        auto bias = parse_gyro_bias(controller.read_memory(0x13040, 16));
        if (bias) {
            motion.emplace(*bias);
            if constexpr (std::is_same_v<Controller, BleController>) {
                try {
                    const auto config = parse_packed_motion_config(controller.motion_configuration());
                    if (!config) throw std::runtime_error("Invalid packed motion configuration");
                    controller.enable_packed_motion();
                    packed.emplace(*config, *bias);
                } catch (const BleController::Cancelled &) { throw; }
                catch (...) {
                    log_("Packed Bluetooth motion unavailable; using common reports");
                }
            }
        } else
            log_("Invalid factory gyro calibration");
    } catch (const BleController::Cancelled &) { throw; }
    catch (...) {
        log_("Cannot read factory gyro calibration");
    }
    try {
        while (!stop.stop_requested()) {
            auto bytes = controller.read(20);
            const auto now = Clock::now();
            const bool packed_packet = transport == Transport::Bluetooth && packed &&
                                       bytes.size() == 64 && bytes[0] == 9;
            auto state = transport == Transport::Usb || packed_packet ? decode_usb(bytes) : decode_ble(bytes);
            if (state) {
                state->profile = settings_.profile();
                settings_.update(*state, *state, calibration, 1, false, uint8_t(transport));
                FILETIME utc;
                GetSystemTimePreciseAsFileTime(&utc);
                const uint64_t arrival = (uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
                std::optional<MotionBatch> batch;
                if (packed_packet && bytes[15] <= 40) {
                    const auto host_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                    batch = packed->process(std::span(bytes).subspan(16, bytes[15]), host_us, arrival);
                }
                if (batch && batch->count) {
                    for (size_t i = 0; i < batch->count; ++i) {
                        state->motion = batch->samples[i];
                        settings_.update_motion(state->motion);
                        publish_(*state, calibration, transport, i == 0);
                    }
                } else {
                    if (!packed_packet && motion) {
                        state->motion = motion->process(state->raw_motion, now);
                        if (state->motion.ready) state->motion.sample_time_100ns = arrival;
                    }
                    settings_.update_motion(state->motion);
                    publish_(*state, calibration, transport, true);
                }
                last_input = now;
            }
            if (now - last_input > std::chrono::seconds(2))
                throw std::runtime_error("Input stream stalled");
            if (now - last_rumble >= std::chrono::milliseconds(20)) {
                const auto motors = rumble_.load();
                if (motors || vibrated) {
                    if constexpr (std::is_same_v<Controller, UsbController>)
                        controller.write(
                            usb_rumble(sequence++, uint8_t(motors), uint8_t(motors >> 8)));
                    else
                        controller.write(
                            ble_rumble(sequence++, uint8_t(motors), uint8_t(motors >> 8)));
                }
                vibrated = motors != 0;
                last_rumble = now;
            }
        }
    } catch (...) {
        if (vibrated) {
            try {
                if constexpr (std::is_same_v<Controller, UsbController>)
                    controller.write(usb_rumble(sequence, 0, 0));
                else
                    controller.write(ble_rumble(sequence, 0, 0));
            } catch (...) {
            }
        }
        throw;
    }
    if (vibrated) {
        if constexpr (std::is_same_v<Controller, UsbController>)
            controller.write(usb_rumble(sequence, 0, 0));
        else
            controller.write(ble_rumble(sequence, 0, 0));
    }
}
void Session::run(std::stop_token stop, uint64_t address) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        log_("Cannot initialize Windows Runtime");
        return;
    }
    uint64_t previous_address = address;
    bool initial_adoption = true;
    while (!stop.stop_requested()) {
        address = configured_pro_address(address);
        if (address != previous_address) initial_adoption = true;
        previous_address = address;
        bool switching_to_usb = false;
        try {
            const auto usb = inspect_usb();
            if (!usb.empty()) {
                if (usb.size() != 1)
                    throw std::runtime_error("This prototype supports one physical controller");
                log_("trace: USB initializing");
                initial_adoption = false;
                UsbController controller(usb.front());
                controller.initialize();
                std::vector<uint8_t> identity;
                try { identity = controller.read_memory(0x13000, 32); } catch (...) {}
                settings_.connect(ControllerKind::Pro, identity, 0);
                log_("trace: USB calibration");
                auto calibration = controller.calibration();
                log_("USB input connected");
                pump(controller, calibration, Transport::Usb, stop);
            } else if (address) {
                log_("trace: Bluetooth waiting/connecting");
                uint64_t last_usb_check = 0;
                auto cancelled = [&] {
                    if (stop.stop_requested() || switching_to_usb) return true;
                    const auto now = GetTickCount64();
                    if (now - last_usb_check >= 100) {
                        last_usb_check = now;
                        if (configured_pro_address(address) != address) return true;
                        switching_to_usb = !inspect_usb().empty();
                        if (switching_to_usb) log_("trace: USB arrived during Bluetooth");
                    }
                    return switching_to_usb;
                };
                BleController controller({address, false}, {
                    .wait_until_available = true,
                    .reuse_windows_connection = std::exchange(initial_adoption, false),
                    .cancelled = cancelled, .trace = log_});
                log_("trace: Bluetooth initializing");
                controller.initialize(false);
                std::vector<uint8_t> identity;
                try { identity = controller.read_memory(0x13000, 32); } catch (...) {}
                settings_.connect(ControllerKind::Pro, identity, address);
                log_("trace: Bluetooth calibration");
                Calibration calibration;
                for (size_t side = 0; side < 2; ++side) {
                    auto user = controller.read_memory(uint32_t(0x1fc040 + 64 * side), 11);
                    auto factory = controller.read_memory(uint32_t(0x130a8 + 64 * side), 9);
                    auto data = user[0] == 0xb2 && user[1] == 0xa1 ? std::span(user).subspan(2)
                                                                   : std::span(factory);
                    if (!parse_calibration(data, calibration, side * 2))
                        throw std::runtime_error("Invalid Bluetooth stick calibration");
                }
                log_("Bluetooth input connected");
                pump(controller, calibration, Transport::Bluetooth, stop);
            }
        } catch (const BleController::Cancelled &) {
            if (switching_to_usb) log_("trace: Switching to USB");
        } catch (const winrt::hresult_error &error) {
            log_("Bluetooth: " + winrt::to_string(error.message()));
        } catch (const std::exception &error) {
            log_(error.what());
        } catch (...) {
            log_("Unexpected controller error");
        }
        // Never leave a pressed button or active motor latched after loss of input.
        rumble_.store(0);
        settings_.disconnect();
        publish_(State{}, Calibration{}, Transport::Disconnected, false);
        if (!switching_to_usb && !stop.stop_requested()) Sleep(100);
    }
    winrt::uninit_apartment();
}
} // namespace switch2
