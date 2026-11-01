// GPL-3.0. HID report conventions follow aisk/WinStadia; see THIRD_PARTY_NOTICES.md.
#include <windows.h>
#include <wdf.h>
#include <hidport.h>
#include "session.hpp"
#include "report_descriptor.hpp"
#include "sensor_reports.hpp"
#include "motion_snapshot.hpp"
#include "joy_reader.hpp"
#include "joy_session.hpp"
#include "gamepad_reports.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <new>

struct DeviceContext {
    SRWLOCK lock;
    WDFQUEUE reads;
    switch2::Session *session;
    BOOLEAN sensor_only, joy_hub, pro_hub;
    switch2::JoyFrame pro_frame;
    switch2::JoyStatus pro_status;
    switch2::ProConnectionTrace pro_trace;
    UCHAR joy_slot; // 1 through 4 for Joy-Con gamepad and motion roots
    switch2::JoySession *joy_session;
    JoyReader *joy_reader;
    switch2::ReportQueue<switch2::JoyFrame, 128> joy_reports;
    switch2::ReportQueue<switch2::MouseReport, 128> mouse_reports;
    switch2::MouseReport mouse_state;
    switch2::MotionSnapshot motion_snapshot;
    switch2::ReportQueue<switch2::MotionSnapshot, 64> motion_reports;
    switch2::MotionDeliveryStatistics delivery;
    ULONG last_motion_sequence;
    BOOLEAN have_motion_sequence;
    UCHAR controls[16];
    BOOLEAN gamepad_changed;
    BOOLEAN home_changed;
    UCHAR transport;
    ULONG reports, errors;
    char last_error[48];
    sensor_hid::Channel sensors[2];
    unsigned next_sensor;
    UCHAR last_set_feature[32];
    ULONG feature_writes;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, context_of)
extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD device_add;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL device_control;
EVT_WDF_DEVICE_D0_ENTRY power_up;
EVT_WDF_DEVICE_D0_EXIT power_down;
EVT_WDF_OBJECT_CONTEXT_CLEANUP cleanup_device;

static const HID_DEVICE_ATTRIBUTES device_attributes = {sizeof(HID_DEVICE_ATTRIBUTES), 0x045e,
                                                        0x02fd, 0x0903};
static std::span<const uint8_t> report_descriptor(DeviceContext *context) {
    if (context->joy_hub) return std::span(JoyMouseDescriptor.bytes).first(JoyMouseDescriptor.length);
    if (context->pro_hub) return JoyHubDescriptor;
    if (!context->sensor_only) return GamepadDescriptor;
    const auto &descriptor = JoyHomeDescriptor;
    return std::span(descriptor.bytes).first(descriptor.length);
}
static uint64_t report_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static NTSTATUS copy_result(WDFREQUEST request, const void *data, size_t length) {
    void *output = nullptr;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, length, &output, nullptr);
    if (NT_SUCCESS(status)) {
        std::memcpy(output, data, length);
        WdfRequestSetInformation(request, length);
    }
    return status;
}
static size_t make_report(DeviceContext *context, UCHAR id, UCHAR *output) {
    output[0] = id;
    if (id == 0xed && context->joy_hub) {
        auto report = context->mouse_state;
        report.dx = report.dy = 0;
        report.wheel = 0;
        std::memcpy(output, &report, sizeof(report));
        return sizeof(report);
    }
    if (id == 0x10 || id == 0x11) {
        auto report = context->sensors[id - 0x10].input;
        report.event = 5; // Poll response
        std::memcpy(output, &report, sizeof(report));
        return sizeof(report);
    }
    if (id == 1) {
        std::array<uint8_t, 16> controls;
        std::memcpy(controls.data(), context->controls, controls.size());
        const auto report = switch2::gamepad_report(controls);
        std::memcpy(output, report.data(), report.size());
        return report.size();
    }
    if (id == 5 && context->sensor_only) {
        std::array<uint8_t, 16> controls;
        std::memcpy(controls.data(), context->controls, controls.size());
        const auto report = switch2::home_button_report(controls);
        std::memcpy(output, report.data(), report.size());
        return report.size();
    }
    // Battery telemetry is not validated yet; do not invent a charge level.
    if (id == 4) {
        output[1] = 0;
        return 2;
    }
    return 0;
}
static size_t next_report(DeviceContext *context, UCHAR *output) {
    if (!context->mouse_reports.empty()) {
        std::memcpy(output, &context->mouse_reports.front(), sizeof(switch2::MouseReport));
        context->mouse_reports.pop();
        return sizeof(switch2::MouseReport);
    }
    if (!context->joy_reports.empty()) {
        std::memcpy(output, &context->joy_reports.front(), sizeof(switch2::JoyFrame));
        context->joy_reports.pop();
        return sizeof(switch2::JoyFrame);
    }
    if (context->gamepad_changed) {
        context->gamepad_changed = FALSE;
        return make_report(context, 1, output);
    }
    if (context->home_changed) {
        context->home_changed = FALSE;
        return make_report(context, 5, output);
    }
    if (!context->motion_reports.empty()) {
        std::memcpy(output, &context->motion_reports.front(), sizeof(switch2::MotionSnapshot));
        context->motion_reports.pop();
        return sizeof(switch2::MotionSnapshot);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned index = (context->next_sensor + i) % 2;
        auto &sensor = context->sensors[index];
        if (!sensor.pending)
            continue;
        std::memcpy(output, &sensor.reports.front(), sizeof(sensor.input));
        sensor.sent(report_time_ms());
        context->next_sensor = (index + 1) % 2;
        return sizeof(sensor.input);
    }
    return 0;
}
static void publish_controls(DeviceContext *context, const std::array<uint8_t, 16> &controls,
                             const switch2::Motion &motion, switch2::Transport transport) {
    AcquireSRWLockExclusive(&context->lock);
    if (std::memcmp(context->controls, controls.data(), 14) ||
        (context->controls[14] & 3) != (controls[14] & 3))
        context->gamepad_changed = TRUE;
    std::copy(controls.begin(), controls.end(), context->controls);
    bool motion_changed = transport == switch2::Transport::Disconnected && context->transport != 0;
    context->transport = UCHAR(transport);
    context->motion_snapshot.transport = UCHAR(transport);
    if (transport == switch2::Transport::Disconnected) {
        const auto sequence = context->motion_snapshot.sequence;
        context->motion_snapshot = {};
        context->motion_snapshot.sequence = sequence;
    }
    else if (motion.ready) {
        context->motion_snapshot.ready = 1;
        context->motion_snapshot.updated_ms = GetTickCount64();
        ++context->motion_snapshot.sequence;
        context->motion_snapshot.angular_velocity = motion.angular_velocity;
        context->motion_snapshot.acceleration = motion.acceleration;
        context->motion_snapshot.sample_time_100ns = motion.sample_time_100ns;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        context->motion_snapshot.published_qpc = now.QuadPart;
        motion_changed = true;
    }
    if (motion_changed) {
        auto report = context->motion_snapshot;
        report.id = 0xe4;
        if (!report.ready)
            context->motion_reports.clear();
        context->motion_reports.push(report);
    }
    if (transport != switch2::Transport::Disconnected)
        ++context->reports;
    while (context->gamepad_changed || !context->motion_reports.empty() ||
           context->sensors[0].pending || context->sensors[1].pending) {
        WDFREQUEST request;
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(context->reads, &request)))
            break;
        UCHAR report[64]{};
        auto length = next_report(context, report);
        WdfRequestComplete(request, copy_result(request, report, length));
    }
    ReleaseSRWLockExclusive(&context->lock);
}
static void publish_joy(DeviceContext *context, const switch2::JoyFrame &frame) {
    switch2::Motion motion;
    motion.ready = (frame.flags & 2) != 0;
    motion.angular_velocity = frame.gyro;
    motion.acceleration = frame.acceleration;
    motion.sample_time_100ns = frame.sample_time_100ns;
    publish_controls(context, frame.controls, motion,
                     (frame.flags & 1) ? ((frame.flags & 4) ? switch2::Transport::Usb : switch2::Transport::Bluetooth) : switch2::Transport::Disconnected);
}
static void queue_joy_frame_locked(DeviceContext *context, const switch2::JoyFrame &frame) {
    if (!(frame.flags & switch2::JoyConnected)) {
        // A disconnected slot must not replay its old buttons or motion when a
        // reader catches up. Keep queued samples belonging to other controllers.
        context->joy_reports.remove_if([&](const auto &queued) { return queued.slot == frame.slot; });
    }
    context->joy_reports.push(frame);
    while (!context->joy_reports.empty() || !context->mouse_reports.empty()) {
        WDFREQUEST request;
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(context->reads, &request))) break;
        UCHAR report[64]{};
        const auto length = next_report(context, report);
        WdfRequestComplete(request, copy_result(request, report, length));
    }
}
static void publish_joy_hub(DeviceContext *context, const switch2::JoyFrame &frame) {
    AcquireSRWLockExclusive(&context->lock);
    queue_joy_frame_locked(context, frame);
    ReleaseSRWLockExclusive(&context->lock);
}
static void publish_mouse(DeviceContext *context, const switch2::MouseReport &report, bool clear_pending) {
    AcquireSRWLockExclusive(&context->lock);
    if (clear_pending) context->mouse_reports.clear();
    context->mouse_state = report;
    context->mouse_reports.push(report);
    while (!context->mouse_reports.empty() || !context->joy_reports.empty()) {
        WDFREQUEST request;
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(context->reads, &request))) break;
        UCHAR bytes[64]{};
        const auto length = next_report(context, bytes);
        WdfRequestComplete(request, copy_result(request, bytes, length));
    }
    ReleaseSRWLockExclusive(&context->lock);
}
static void publish_pro_hub(DeviceContext *context, const switch2::State &state,
                            const switch2::Calibration &calibration, switch2::Transport transport,
                            bool physical_report) {
    switch2::JoyFrame frame;
    frame.controls = switch2::xbox_controls(state, calibration);
    frame.updated_ms = GetTickCount64();
    frame.flags = transport == switch2::Transport::Disconnected ? 0 : 1;
    if (state.motion.ready) frame.flags |= 2;
    if (transport == switch2::Transport::Usb) frame.flags |= 4;
    frame.gyro = state.motion.angular_velocity;
    frame.acceleration = state.motion.acceleration;
    frame.sample_time_100ns = state.motion.sample_time_100ns;
    AcquireSRWLockExclusive(&context->lock);
    frame.sequence = context->pro_frame.sequence + 1;
    context->pro_frame = frame;
    context->pro_status.connected = (frame.flags & 1) != 0;
    if ((frame.flags & 1) && physical_report) ++context->pro_status.reports[0];
    queue_joy_frame_locked(context, frame);
    ReleaseSRWLockExclusive(&context->lock);
}
static void publish_home(DeviceContext *context, const switch2::JoyFrame &frame) {
    const UCHAR home = (frame.flags & 1) ? (frame.controls[14] & 4) : 0;
    AcquireSRWLockExclusive(&context->lock);
    if (context->controls[14] != home) {
        context->controls[14] = home;
        context->home_changed = TRUE;
    }
    while (context->home_changed) {
        WDFREQUEST request;
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(context->reads, &request))) break;
        UCHAR report[64]{};
        const auto length = next_report(context, report);
        WdfRequestComplete(request, copy_result(request, report, length));
    }
    ReleaseSRWLockExclusive(&context->lock);
}
static NTSTATUS read_report(DeviceContext *context, WDFREQUEST request, bool &complete) {
    UCHAR report[64]{};
    AcquireSRWLockExclusive(&context->lock);
    auto length = next_report(context, report);
    if (length) {
        ReleaseSRWLockExclusive(&context->lock);
        return copy_result(request, report, length);
    }
    auto status = WdfRequestForwardToIoQueue(request, context->reads);
    ReleaseSRWLockExclusive(&context->lock);
    complete = !NT_SUCCESS(status);
    return status;
}
static NTSTATUS get_input(WDFREQUEST request, size_t minimum, UCHAR *&data, size_t &length) {
    void *buffer = nullptr;
    auto status = WdfRequestRetrieveInputBuffer(request, minimum, &buffer, &length);
    data = static_cast<UCHAR *>(buffer);
    return status;
}
static NTSTATUS write_report(DeviceContext *context, WDFREQUEST request) {
    UCHAR *data = nullptr;
    size_t length = 0;
    auto status = get_input(request, 6, data, length);
    if (!NT_SUCCESS(status))
        return status;
    if (data[0] != 3)
        return STATUS_INVALID_PARAMETER;
    const UCHAR strong = (data[1] & 2) ? UCHAR(std::min(100, int(data[4])) * 255 / 100) : 0;
    const UCHAR weak = (data[1] & 1) ? UCHAR(std::min(100, int(data[5])) * 255 / 100) : 0;
    AcquireSRWLockShared(&context->lock);
    if (context->session)
        context->session->set_rumble(strong, weak);
    if (context->joy_reader)
        context->joy_reader->set_rumble(strong, weak);
    ReleaseSRWLockShared(&context->lock);
    WdfRequestSetInformation(request, length);
    return STATUS_SUCCESS;
}
static NTSTATUS get_feature(DeviceContext *context, WDFREQUEST request) {
    UCHAR *data = nullptr;
    size_t length = 0;
    auto status = get_input(request, 1, data, length);
    if (!NT_SUCCESS(status))
        return status;
    if (context->joy_hub || context->pro_hub) {
        UCHAR report[64]{};
        bool accepted = false;
        AcquireSRWLockShared(&context->lock);
        if (data[0] >= 0xf0 && data[0] <= 0xf7) {
            const unsigned device = (data[0] - 0xf0) % 4;
            auto *settings = context->joy_session ? &context->joy_session->settings(device) :
                (context->session && device == 0 ? &context->session->settings() : nullptr);
            if (settings) {
                if (data[0] < 0xf4) {
                    const auto value = settings->snapshot();
                    std::memcpy(report, &value, sizeof(value));
                } else {
                    const auto value = settings->motion();
                    std::memcpy(report, &value, sizeof(value));
                }
                report[0] = data[0];
                accepted = true;
            }
        }
        if (context->joy_session) {
            if (data[0] == 0xe7) {
                const auto value = context->joy_session->status();
                std::memcpy(report, &value, sizeof(value));
                accepted = true;
            } else if (const auto found = std::find(switch2::JoySnapshotIds.begin(), switch2::JoySnapshotIds.end(), data[0]);
                       found != switch2::JoySnapshotIds.end()) {
                auto value = context->joy_session->latest(unsigned(found - switch2::JoySnapshotIds.begin()));
                value.id = data[0];
                std::memcpy(report, &value, sizeof(value));
                accepted = true;
            }
        }
        if (context->pro_hub) {
            if (data[0] == 0xe7) {
                std::memcpy(report, &context->pro_status, sizeof(context->pro_status));
                accepted = true;
            } else if (data[0] == 0xe9) {
                std::memcpy(report, &context->pro_trace, sizeof(context->pro_trace));
                report[0] = 0xe9;
                report[1] = 1;
                accepted = true;
            } else if (data[0] == 0xe8) {
                auto frame = context->pro_frame;
                frame.id = 0xe8;
                std::memcpy(report, &frame, sizeof(frame));
                accepted = true;
            }
        }
        ReleaseSRWLockShared(&context->lock);
        return accepted ? copy_result(request, report, sizeof(report)) : STATUS_INVALID_PARAMETER;
    }
    if (data[0] == 0xe5 && context->sensor_only) {
        AcquireSRWLockShared(&context->lock);
        auto report = context->delivery;
        report.gyro_dropped = context->sensors[0].reports.dropped;
        report.accel_dropped = context->sensors[1].reports.dropped;
        ReleaseSRWLockShared(&context->lock);
        return copy_result(request, &report, sizeof(report));
    }
    if (data[0] == 0xe3 && context->sensor_only) {
        UCHAR report[96]{0xe3, 1};
        AcquireSRWLockShared(&context->lock);
        std::memcpy(report + 2, &context->reports, 4);
        for (size_t i = 0; i < 2; ++i) {
            std::memcpy(report + 6 + i * 17, &context->sensors[i].feature, 17);
            std::memcpy(report + 40 + i * 12, context->sensors[i].input.axes, 12);
        }
        std::memcpy(report + 64, context->last_set_feature, 32);
        ReleaseSRWLockShared(&context->lock);
        return copy_result(request, report, sizeof(report));
    }
    if (data[0] == 0xe2 && !context->sensor_only) {
        AcquireSRWLockShared(&context->lock);
        auto snapshot = context->motion_snapshot;
        ReleaseSRWLockShared(&context->lock);
        return copy_result(request, &snapshot, sizeof(snapshot));
    }
    if (data[0] == 0x10 || data[0] == 0x11) {
        AcquireSRWLockShared(&context->lock);
        auto report = context->sensors[data[0] - 0x10].feature;
        ReleaseSRWLockShared(&context->lock);
        return copy_result(request, &report, sizeof(report));
    }
    if (data[0] != 0xe1)
        return STATUS_INVALID_PARAMETER;
    UCHAR report[64]{0xe1, 1};
    AcquireSRWLockShared(&context->lock);
    report[2] = context->transport;
    std::memcpy(report + 4, &context->reports, sizeof(ULONG));
    std::memcpy(report + 8, &context->errors, sizeof(ULONG));
    std::memcpy(report + 12, context->last_error, sizeof(context->last_error));
    std::memcpy(report + 60, &context->motion_reports.dropped, 4);
    ReleaseSRWLockShared(&context->lock);
    return copy_result(request, report, sizeof(report));
}
static NTSTATUS set_feature(DeviceContext *context, WDFREQUEST request) {
    UCHAR *data = nullptr;
    size_t length = 0;
    auto status = get_input(request, sizeof(sensor_hid::FeatureReport), data, length);
    if (!NT_SUCCESS(status))
        return status;
    if (context->joy_hub || context->pro_hub) {
        if (length != sizeof(switch2::JoyRequest)) return STATUS_INVALID_PARAMETER;
        switch2::JoyRequest command;
        std::memcpy(&command, data, sizeof(command));
        WDF_REQUEST_PARAMETERS parameters;
        WDF_REQUEST_PARAMETERS_INIT(&parameters);
        WdfRequestGetParameters(request, &parameters);
        if (parameters.Parameters.DeviceIoControl.OutputBufferLength != 0xea)
            return STATUS_INVALID_PARAMETER;
        AcquireSRWLockShared(&context->lock);
        bool accepted = context->joy_session && context->joy_session->request(command);
        if (context->pro_hub && context->session && command.value == 0 &&
            command.command == switch2::JoyCommand::Profile)
            accepted = context->session->settings().apply(command);
        if (context->pro_hub && context->session && command.version == 2 &&
            command.command == switch2::JoyCommand::Rumble && command.value == 0) {
            context->session->set_rumble(command.strong, command.weak);
            accepted = true;
        }
        ReleaseSRWLockShared(&context->lock);
        if (accepted) WdfRequestSetInformation(request, sizeof(command));
        return accepted ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    sensor_hid::FeatureReport report;
    std::memcpy(&report, data, sizeof(report));
    // UMDF receives the authoritative HID_XFER_PACKET report ID in the
    // output-length parameter. Internal sensor clients may leave buffer[0] zero.
    WDF_REQUEST_PARAMETERS parameters;
    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(request, &parameters);
    report.id = UCHAR(parameters.Parameters.DeviceIoControl.OutputBufferLength);
    AcquireSRWLockExclusive(&context->lock);
    std::memcpy(context->last_set_feature, data, sizeof(report));
    context->last_set_feature[17] = report.id;
    auto input_length = ULONG(length);
    std::memcpy(context->last_set_feature + 18, &input_length, 4);
    bool accepted =
        (report.id == 0x10 || report.id == 0x11) && context->sensors[report.id - 0x10].set(report);
    const NTSTATUS result = accepted ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    std::memcpy(context->last_set_feature + 22, &result, 4);
    ++context->feature_writes;
    std::memcpy(context->last_set_feature + 26, &context->feature_writes, 4);
    ReleaseSRWLockExclusive(&context->lock);
    if (accepted)
        WdfRequestSetInformation(request, sizeof(report));
    return accepted ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
static NTSTATUS get_string(DeviceContext *context, WDFREQUEST request) {
    UCHAR *data = nullptr;
    size_t length = 0;
    auto status = get_input(request, sizeof(ULONG), data, length);
    if (!NT_SUCCESS(status))
        return status;
    ULONG id = 0;
    std::memcpy(&id, data, sizeof(id));
    switch (id & 0xffff) {
    case HID_STRING_ID_IMANUFACTURER: {
        const WCHAR value[] = L"Switch2Native";
        return copy_result(request, value, sizeof(value));
    }
    case HID_STRING_ID_IPRODUCT: {
        if (context->joy_hub || context->pro_hub || context->joy_slot) {
            const WCHAR *value = context->pro_hub ? L"Switch 2 Pro Controller Coordinator" : context->joy_hub ? L"Switch 2 Joy-Con Coordinator" :
                context->sensor_only ? L"Switch 2 Joy-Con Motion" : L"Switch 2 Joy-Con (XInput)";
            return copy_result(request, value, (wcslen(value) + 1) * sizeof(WCHAR));
        }
        if (context->sensor_only) {
            const WCHAR value[] = L"Switch 2 Pro Controller Motion";
            return copy_result(request, value, sizeof(value));
        }
        const WCHAR value[] = L"Switch 2 Pro Controller (XInput)";
        return copy_result(request, value, sizeof(value));
    }
    case HID_STRING_ID_ISERIALNUMBER: {
        if (context->joy_hub || context->pro_hub || context->joy_slot) {
            const std::wstring value = context->pro_hub ? L"Switch2Native-ProHub" : context->joy_hub ? L"Switch2Native-JoyHub" :
                (context->sensor_only ? L"Switch2Native-JoyMotion-0" : L"Switch2Native-JoyPad-0") + std::to_wstring(context->joy_slot);
            return copy_result(request, value.c_str(), (value.size() + 1) * sizeof(WCHAR));
        }
        if (context->sensor_only) {
            const WCHAR value[] = L"Switch2Native-Motion-01";
            return copy_result(request, value, sizeof(value));
        }
        const WCHAR value[] = L"Switch2Native-01";
        return copy_result(request, value, sizeof(value));
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}
void device_control(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
    auto context = context_of(WdfIoQueueGetDevice(queue));
    bool complete = true;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    switch (code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        const HID_DESCRIPTOR hid_descriptor = {
            9,
            0x21,
            0x0100,
            0,
            1,
            {{0x22,
              USHORT(report_descriptor(context).size())}}};
        status = copy_result(request, &hid_descriptor, sizeof(hid_descriptor));
        break;
    }
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = copy_result(request, &device_attributes, sizeof(device_attributes));
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        const auto descriptor = report_descriptor(context);
        status = copy_result(request, descriptor.data(), descriptor.size());
        break;
    }
    case IOCTL_HID_READ_REPORT:
        status = read_report(context, request, complete);
        break;
    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
        status = write_report(context, request);
        break;
    case IOCTL_UMDF_HID_GET_FEATURE:
        status = get_feature(context, request);
        break;
    case IOCTL_UMDF_HID_SET_FEATURE:
        status = set_feature(context, request);
        break;
    case IOCTL_HID_GET_STRING:
        status = get_string(context, request);
        break;
    case IOCTL_UMDF_HID_GET_INPUT_REPORT: {
        UCHAR *id = nullptr;
        size_t length = 0;
        status = get_input(request, 1, id, length);
        if (!NT_SUCCESS(status))
            break;
        UCHAR report[64]{};
        AcquireSRWLockShared(&context->lock);
        length = make_report(context, *id, report);
        ReleaseSRWLockShared(&context->lock);
        status = length ? copy_result(request, report, length) : STATUS_INVALID_PARAMETER;
        break;
    }
    }
    if (complete)
        WdfRequestComplete(request, status);
}
NTSTATUS power_up(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto context = context_of(device);
    uint64_t address = 0;
    DWORD size = sizeof(address);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Native", L"BluetoothAddress",
                 RRF_RT_REG_QWORD, nullptr, &address, &size);
    try {
        if (context->joy_hub) {
            AcquireSRWLockExclusive(&context->lock);
            context->mouse_reports.clear();
            context->mouse_state = {};
            context->mouse_reports.push(context->mouse_state);
            ReleaseSRWLockExclusive(&context->lock);
            auto session = new switch2::JoySession(
                [context](auto const &frame) { publish_joy_hub(context, frame); },
                [context](auto const &report, bool clear) { publish_mouse(context, report, clear); });
            AcquireSRWLockExclusive(&context->lock);
            context->joy_session = session;
            ReleaseSRWLockExclusive(&context->lock);
            return STATUS_SUCCESS;
        }
        if (context->sensor_only) {
            context->joy_reader = new JoyReader(context->joy_slot ? context->joy_slot - 1 : 0,
                [context](auto const &frame) { publish_home(context, frame); },
                context->joy_slot ? L"Switch2Native-JoyHub" : L"Switch2Native-ProHub");
            return STATUS_SUCCESS;
        }
        if (!context->pro_hub) {
            auto reader = new JoyReader(context->joy_slot ? context->joy_slot - 1 : 0,
                [context](auto const &frame) { publish_joy(context, frame); },
                context->joy_slot ? L"Switch2Native-JoyHub" : L"Switch2Native-ProHub");
            AcquireSRWLockExclusive(&context->lock);
            context->joy_reader = reader;
            ReleaseSRWLockExclusive(&context->lock);
            return STATUS_SUCCESS;
        }
        auto session = new switch2::Session(
            address,
            [context](auto const &state, auto const &calibration, auto transport, bool physical_report) {
                publish_pro_hub(context, state, calibration, transport, physical_report);
            },
            [context](const std::string &message) {
                OutputDebugStringA(("Switch2Native: " + message + "\n").c_str());
                AcquireSRWLockExclusive(&context->lock);
                if (!message.starts_with("trace:") && message != "USB input connected" && message != "Bluetooth input connected")
                    ++context->errors;
                context->pro_status.errors[0] = context->errors;
                strncpy_s(context->last_error, message.c_str(), _TRUNCATE);
                ++context->pro_trace.sequence;
                context->pro_trace.updated_ms = GetTickCount64();
                strncpy_s(context->pro_trace.message, message.c_str(), _TRUNCATE);
                ReleaseSRWLockExclusive(&context->lock);
            });
        AcquireSRWLockExclusive(&context->lock);
        context->session = session;
        ReleaseSRWLockExclusive(&context->lock);
        return STATUS_SUCCESS;
    } catch (...) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}
static void stop_session(DeviceContext *context) {
    AcquireSRWLockExclusive(&context->lock);
    auto session = context->session;
    context->session = nullptr;
    auto joy_session = context->joy_session;
    context->joy_session = nullptr;
    auto joy_reader = context->joy_reader;
    context->joy_reader = nullptr;
    ReleaseSRWLockExclusive(&context->lock);
    delete session;
    delete joy_reader;
    delete joy_session;
}
NTSTATUS power_down(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto context = context_of(device);
    stop_session(context);
    // Workers have joined, so no late completion can repopulate these queues.
    // A device restart starts from neutral input, not the last powered session.
    AcquireSRWLockExclusive(&context->lock);
    context->joy_reports.clear();
    context->mouse_reports.clear();
    context->motion_reports.clear();
    context->mouse_state = {};
    context->motion_snapshot = {};
    context->pro_frame = {};
    context->pro_status = {};
    context->transport = 0;
    const auto idle = switch2::xbox_controls(switch2::State{}, switch2::Calibration{});
    std::copy(idle.begin(), idle.end(), context->controls);
    context->gamepad_changed = !context->sensor_only && !context->joy_hub && !context->pro_hub;
    context->home_changed = context->sensor_only;
    ReleaseSRWLockExclusive(&context->lock);
    return STATUS_SUCCESS;
}
void cleanup_device(WDFOBJECT object) {
    stop_session(context_of(object));
}
NTSTATUS device_add(WDFDRIVER, PWDFDEVICE_INIT initialization) {
    WdfFdoInitSetFilter(initialization);
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = power_up;
    power.EvtDeviceD0Exit = power_down;
    WdfDeviceInitSetPnpPowerEventCallbacks(initialization, &power);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = cleanup_device;
    WDFDEVICE device;
    auto status = WdfDeviceCreate(&initialization, &attributes, &device);
    if (!NT_SUCCESS(status))
        return status;
    auto context = context_of(device);
    WCHAR hardware[128]{};
    ULONG required = 0;
    status = WdfDeviceQueryProperty(device, DevicePropertyHardwareID, sizeof(hardware), hardware,
                                    &required);
    if (!NT_SUCCESS(status))
        return status;
    context->sensor_only = _wcsicmp(hardware, L"ROOT\\SWITCH2MOTION") == 0;
    context->joy_hub = _wcsicmp(hardware, L"ROOT\\SWITCH2JOYHUB") == 0;
    context->pro_hub = _wcsicmp(hardware, L"ROOT\\SWITCH2PROHUB") == 0;
    context->pro_frame = {};
    context->pro_status = {};
    for (UCHAR slot = 1; slot <= switch2::MaxJoyCons; ++slot) {
        const auto suffix = std::to_wstring(slot);
        if (_wcsicmp(hardware, (L"ROOT\\SWITCH2JOYPAD" + suffix).c_str()) == 0) context->joy_slot = slot;
        if (_wcsicmp(hardware, (L"ROOT\\SWITCH2JOYMOTION" + suffix).c_str()) == 0) {
            context->joy_slot = slot;
            context->sensor_only = TRUE;
        }
    }
    context->joy_reports = {};
    context->mouse_reports = {};
    context->mouse_state = {};
    context->motion_snapshot = switch2::MotionSnapshot{};
    context->motion_reports = {};
    context->delivery = {};
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    context->delivery.qpc_frequency = frequency.QuadPart;
    InitializeSRWLock(&context->lock);
    auto idle = switch2::xbox_controls(switch2::State{}, switch2::Calibration{});
    std::copy(idle.begin(), idle.end(), context->controls);
    context->gamepad_changed = !context->sensor_only && !context->joy_hub && !context->pro_hub;
    context->home_changed = context->sensor_only;
    context->sensors[0].initialize(0x10);
    context->sensors[1].initialize(0x11);
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.EvtIoDeviceControl = device_control;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
        return status;
    WDF_IO_QUEUE_CONFIG_INIT(&queue, WdfIoQueueDispatchManual);
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, &context->reads);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, device_add);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
