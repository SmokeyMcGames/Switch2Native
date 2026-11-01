#include "hid_helpers.hpp"
#include "joy_protocol.hpp"
#include "usb.hpp"
#include <iomanip>

int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        const auto seconds = argc == 2 ? std::stoul(argv[1]) : 90;
        auto hub = switch2::open_hid_collection(0xff00, 3, L"Switch2Native-ProHub", 0);
        if (!hub.valid()) throw std::runtime_error("Pro coordinator unavailable");
        const auto start = GetTickCount64(), end = start + seconds * 1000;
        uint64_t last_usb_check = 0, last_frame = 0, maximum_gap = 0;
        uint32_t sequence = 0, trace_sequence = 0;
        unsigned flags = 255;
        int usb_present = -1;
        auto stamp = [&] { std::cout << "time_ms=" << GetTickCount64() - start << ' '; };
        do {
            const auto now = GetTickCount64();
            if (now - last_usb_check >= 100) {
                const int present = !switch2::inspect_usb().empty();
                if (present != usb_present) { stamp(); std::cout << "usb_present=" << present << '\n'; }
                usb_present = present;
                last_usb_check = now;
            }
            switch2::ProConnectionTrace trace;
            if (HidD_GetFeature(hub.get(), &trace, sizeof(trace)) && trace.version == 1 && trace.sequence != trace_sequence) {
                trace.message[sizeof(trace.message) - 1] = 0;
                stamp();
                std::cout << "stage_age_ms=" << GetTickCount64() - trace.updated_ms << " stage=" << trace.message << '\n';
                trace_sequence = trace.sequence;
            }
            switch2::JoyFrame frame;
            frame.id = 0xe8;
            if (!HidD_GetFeature(hub.get(), &frame, sizeof(frame))) throw std::runtime_error("Cannot read Pro status");
            if (frame.flags != flags) {
                stamp(); std::cout << "flags=" << unsigned(frame.flags) << " frame_age_ms=" << GetTickCount64() - frame.updated_ms << '\n';
                flags = frame.flags;
            }
            if ((frame.flags & 1) && frame.sequence != sequence) {
                if (last_frame && frame.updated_ms >= last_frame) {
                    const auto gap = frame.updated_ms - last_frame;
                    maximum_gap = std::max(maximum_gap, gap);
                    if (gap > 100) { stamp(); std::cout << "input_gap_ms=" << gap << '\n'; }
                }
                last_frame = frame.updated_ms;
            }
            sequence = frame.sequence;
            Sleep(10);
        } while (GetTickCount64() < end);
        std::cout << "maximum_input_gap_ms=" << maximum_gap << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
