#include "usb.hpp"
#include "usb_descriptors.hpp"
#include <hidsdi.h>
#include <cstring>
#include <iomanip>
#include <set>
#include <map>
#include <memory>
#include <fstream>

// The optional report-09 experiment changes only the volatile input format.
// Restore the driver's normal report 05 on normal completion and on errors.
class UsbReportMode {
  public:
    UsbReportMode() {
        const GUID vendor{
            0x6f13725e, 0xef0e, 0x4fd3, {0xae, 0x5f, 0xb2, 0xde, 0x98, 0x9e, 0xc8, 0x25}};
        for (const auto &path : switch2::device_paths(vendor)) {
            if (!switch2::is_controller(path))
                continue;
            file_.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr));
            if (!file_.valid())
                switch2::fail("Open USB command interface");
            WINUSB_INTERFACE_HANDLE raw = nullptr;
            if (!WinUsb_Initialize(file_.get(), &raw))
                switch2::fail("Initialize USB command interface");
            usb_.reset(raw);
            USB_INTERFACE_DESCRIPTOR descriptor{};
            if (!WinUsb_QueryInterfaceSettings(raw, 0, &descriptor))
                switch2::fail("Query USB command interface");
            for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
                WINUSB_PIPE_INFORMATION pipe{};
                if (!WinUsb_QueryPipe(raw, 0, i, &pipe))
                    switch2::fail("Query USB command pipe");
                if (pipe.PipeType == UsbdPipeTypeBulk)
                    (pipe.PipeId & 0x80 ? input_ : output_) = pipe.PipeId;
            }
            if (!input_ || !output_)
                throw std::runtime_error("USB command endpoints unavailable");
            ULONG timeout = 1000;
            for (auto pipe : {input_, output_})
                if (!WinUsb_SetPipePolicy(raw, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout),
                                          &timeout))
                    switch2::fail("Set USB command timeout");
            return;
        }
        throw std::runtime_error("USB command interface not found");
    }
    void alternate() {
        restore_ = true;
        select(9);
    }
    void normal() {
        select(5);
    }
    void motion_info() {
        for (const auto id : {uint8_t(0x10), uint8_t(0x11)}) {
            auto reply = exchange({id, 0x91, 0, uint8_t(id == 0x10 ? 1 : 3), 0, 0, 0, 0});
            std::cout << "info_command=" << std::hex << unsigned(id) << " response=";
            for (auto value : reply)
                std::cout << std::setfill('0') << std::setw(2) << unsigned(value);
            std::cout << std::dec << '\n';
        }
    }
    void usb_info() {
        UCHAR speed = 0;
        ULONG length = sizeof(speed);
        if (!WinUsb_QueryDeviceInformation(usb_.get(), DEVICE_SPEED, &length, &speed))
            switch2::fail("Query USB device speed");
        std::array<UCHAR, 4096> bytes{};
        ULONG received = 0;
        if (!WinUsb_GetDescriptor(usb_.get(), USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, bytes.data(),
                                  ULONG(bytes.size()), &received))
            switch2::fail("Read USB configuration descriptor");
        std::cout << "usb_speed_code=" << unsigned(speed) << " configuration_bytes=" << received
                  << '\n';
        switch2::describe_usb_configuration(std::span(bytes).first(received), std::cout);
        switch2::describe_controller_usb_port(std::cout);
    }
    void restore() {
        if (restore_) {
            select(5);
            restore_ = false;
            std::cout << "Restored input report 05\n";
        }
    }
    ~UsbReportMode() {
        try {
            restore();
        } catch (const std::exception &error) {
            std::cerr << "Report restore failed; reconnect USB: " << error.what() << '\n';
        }
    }

  private:
    void select(UCHAR id) {
        exchange({3, 0x91, 0, 0x0a, 0, 4, 0, 0, id, 0, 0, 0});
    }
    std::vector<uint8_t> exchange(std::vector<uint8_t> command) {
        ULONG count = 0;
        if (!WinUsb_WritePipe(usb_.get(), output_, command.data(), ULONG(command.size()), &count,
                              nullptr) ||
            count != command.size())
            switch2::fail("Write USB motion probe command");
        UCHAR reply[64]{};
        if (!WinUsb_ReadPipe(usb_.get(), input_, reply, sizeof(reply), &count, nullptr))
            switch2::fail("Read USB motion probe acknowledgement");
        if (count < 8 || reply[0] != command[0] || reply[1] != 1 || reply[3] != command[3])
            throw std::runtime_error("Unexpected USB motion probe acknowledgement");
        return {reply, reply + count};
    }
    switch2::Handle file_;
    std::unique_ptr<void, decltype(&WinUsb_Free)> usb_{nullptr, WinUsb_Free};
    UCHAR input_{}, output_{};
    bool restore_{};
};

// The default and --control paths are read-only.
int main(int argc, char **argv) {
    try {
        bool control = false, alternate = false, compare = false, info = false, usb_info = false;
        unsigned seconds = 5;
        std::string capture_path;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--control")
                control = true;
            else if (argument == "--report-09")
                alternate = true;
            else if (argument == "--compare-motion")
                compare = true;
            else if (argument == "--motion-info")
                info = true;
            else if (argument == "--usb-info")
                usb_info = true;
            else if (argument == "--seconds" && i + 1 < argc)
                seconds = std::stoul(argv[++i]);
            else if (argument == "--capture" && i + 1 < argc)
                capture_path = argv[++i];
            else
                throw std::runtime_error(
                    "Usage: UsbRateProbe [--control | --report-09 | --compare-motion] "
                    "[--motion-info] [--usb-info] [--seconds 1..120] [--capture FILE]");
        }
        if (!seconds || seconds > 120)
            throw std::runtime_error("Probe duration must be 1..120 seconds");
        if (unsigned(control) + unsigned(alternate) + unsigned(compare) > 1)
            throw std::runtime_error("Choose only one experimental USB path");
        std::ofstream capture;
        if (!capture_path.empty()) {
            capture.exceptions(std::ios::failbit | std::ios::badbit);
            capture.open(capture_path);
            capture << "# host_us report_hex\n";
        }
        const auto devices = switch2::inspect_usb();
        if (devices.size() != 1)
            throw std::runtime_error("Connect exactly one Switch 2 Pro Controller by USB");
        switch2::UsbController stream(devices.front());
        if (info || usb_info) {
            UsbReportMode commands;
            if (info)
                commands.motion_info();
            if (usb_info)
                commands.usb_info();
        }
        std::unique_ptr<UsbReportMode> mode;
        if (alternate || compare) {
            mode = std::make_unique<UsbReportMode>();
            mode->alternate();
        }
        switch2::Handle handle;
        if (control) {
            handle.reset(CreateFileW(devices.front().path.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                     nullptr));
            if (!handle.valid())
                switch2::fail("Open HID control-report reader");
        }
        const auto start = std::chrono::steady_clock::now();
        auto next_mode_change = start + std::chrono::milliseconds(250);
        bool packed_mode = true;
        uint64_t reports = 0;
        uint32_t first_uptime = 0, last_uptime = 0;
        std::set<uint32_t> timestamps;
        std::map<unsigned, unsigned> ids;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            if (compare && std::chrono::steady_clock::now() >= next_mode_change) {
                packed_mode = !packed_mode;
                if (packed_mode)
                    mode->alternate();
                else
                    mode->normal();
                next_mode_change =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            }
            std::vector<uint8_t> report;
            if (control) {
                report.resize(devices.front().input_length);
                report[0] = 5;
                if (!HidD_GetInputReport(handle.get(), report.data(), ULONG(report.size())))
                    switch2::fail("HID GET_REPORT (control transfer)");
            } else {
                report = stream.read(100);
            }
            if (capture.is_open() && !report.empty()) {
                const auto host_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                capture << std::dec << host_us << ' ';
                for (auto value : report)
                    capture << std::hex << std::setfill('0') << std::setw(2) << unsigned(value);
                capture << '\n';
            }
            const auto state = switch2::decode_usb(report);
            if (!state)
                continue;
            ++ids[report[0]];
            if (reports < 3) {
                std::cout << "report=";
                for (auto value : report)
                    std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(value);
                std::cout << std::dec << '\n';
            }
            if (report[0] == 5)
                std::memcpy(&last_uptime, report.data() + 1, 4);
            if (!reports)
                first_uptime = last_uptime;
            ++reports;
            if (state->raw_motion.timestamp)
                timestamps.insert(state->raw_motion.timestamp);
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (mode)
            mode->restore();
        std::cout << std::fixed << std::setprecision(3)
                  << "path=" << (control ? "control" : "interrupt") << " seconds=" << elapsed
                  << " reports=" << reports << " reads_hz=" << reports / elapsed
                  << " distinct_imu=" << timestamps.size()
                  << " distinct_imu_hz=" << timestamps.size() / elapsed
                  << " reports_without_new_decoded_imu=" << reports - timestamps.size()
                  << " controller_elapsed_ms=" << uint32_t(last_uptime - first_uptime) << '\n';
        for (auto [id, count] : ids)
            std::cout << "report_id=" << id << " count=" << count << '\n';
        return reports ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
