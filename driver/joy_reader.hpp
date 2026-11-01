#pragma once
#include "hid_helpers.hpp"
#include "joy_protocol.hpp"
#include <atomic>
#include <functional>
#include <thread>

class JoyReader {
  public:
    using Publish = std::function<void(const switch2::JoyFrame &)>;
    JoyReader(unsigned slot, Publish publish, std::wstring serial = L"Switch2Native-JoyHub")
        : slot_(slot), serial_(std::move(serial)),
          stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!stop_.valid())
            switch2::fail("Create Joy-Con stop event");
        thread_ = std::jthread([this, publish = std::move(publish)] { run(publish); });
    }
    ~JoyReader() {
        SetEvent(stop_.get());
        thread_.join();
    }
    void set_rumble(uint8_t strong, uint8_t weak) {
        rumble_.store(uint16_t(strong | (unsigned(weak) << 8)));
    }

  private:
    bool send_rumble(HANDLE handle, uint16_t value) {
        switch2::JoyRequest request;
        request.command = switch2::JoyCommand::Rumble;
        request.value = uint8_t(slot_);
        request.strong = uint8_t(value);
        request.weak = uint8_t(value >> 8);
        return HidD_SetFeature(handle, &request, sizeof(request)) != FALSE;
    }
    void run(const Publish &publish) {
        switch2::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), handle;
        uint32_t sequence = 0;
        bool have_sequence = false;
        uint16_t last_rumble = 0;
        switch2::JoyFrame neutral;
        neutral.slot = uint8_t(slot_);
        neutral.controls = switch2::xbox_controls(switch2::State{}, switch2::Calibration{});
        if (!event.valid())
            return;
        switch2::MaintenancePause maintenance;
        while (WaitForSingleObject(stop_.get(), 0) == WAIT_TIMEOUT) {
            if (maintenance.active()) {
                if (handle.valid()) {
                    if (last_rumble) send_rumble(handle.get(), 0);
                    handle.reset();
                    publish(neutral);
                }
                have_sequence = false;
                rumble_.store(0);
                last_rumble = 0;
                WaitForSingleObject(stop_.get(), 100);
                continue;
            }
            if (!handle.valid()) {
                try {
                    handle = switch2::open_hid_collection(0xff00, 3, serial_);
                } catch (...) {
                }
                if (!handle.valid()) {
                    WaitForSingleObject(stop_.get(), 250);
                    continue;
                }
                HidD_SetNumInputBuffers(handle.get(), 128);
                switch2::JoyFrame initial;
                initial.id = switch2::JoySnapshotIds.at(slot_);
                have_sequence = false;
                if (HidD_GetFeature(handle.get(), &initial, sizeof(initial)) &&
                    initial.version == 2 && initial.slot == slot_ &&
                    GetTickCount64() - initial.updated_ms < 2000) {
                    publish(initial);
                    sequence = initial.sequence;
                    have_sequence = true;
                }
            }
            switch2::JoyFrame frame;
            if (!switch2::read_hid_packet(handle.get(), event.get(), stop_.get(), &frame,
                                          sizeof(frame))) {
                handle.reset();
                publish(neutral);
                have_sequence = false;
                continue;
            }
            if (frame.id != 0xe6 || frame.version != 2)
                continue;
            if (frame.slot == slot_ &&
                (!have_sequence || (int32_t(frame.sequence - sequence) > 0))) {
                if (GetTickCount64() - frame.updated_ms < 2000)
                    publish(frame);
                else
                    publish(neutral);
                sequence = frame.sequence;
                have_sequence = true;
            }
            const auto value = rumble_.load();
            if (value != last_rumble) {
                if (send_rumble(handle.get(), value))
                    last_rumble = value;
            }
        }
        if (handle.valid() && last_rumble)
            send_rumble(handle.get(), 0);
    }
    unsigned slot_;
    std::wstring serial_;
    switch2::Handle stop_;
    std::atomic<uint16_t> rumble_{};
    std::jthread thread_;
};
