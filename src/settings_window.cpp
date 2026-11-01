#include "branding.hpp"
#include "settings_window.hpp"
#include "settings_graphics.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>
#include <iomanip>

namespace switch2 {
namespace {
namespace ipc = joy_ipc;
namespace graphics = settings_graphics;
constexpr UINT Updated = WM_APP + 42;
constexpr int PanelHeight = 396, HeaderHeight = 100;
constexpr wchar_t ClassName[] = L"Switch2Native.Controller.Settings";
std::wstring number(double value, int precision = 1) {
    std::wostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}
std::wstring name(const ControllerSnapshot &controller) {
    if (controller.kind == ControllerKind::Pro)
        return L"Switch 2 Pro Controller";
    return controller.kind == ControllerKind::JoyRight ? L"Joy-Con 2 (R)" : L"Joy-Con 2 (L)";
}
std::wstring identity(const ControllerSnapshot &controller) {
    std::wstring text;
    for (char c : controller.serial) {
        if (!c)
            break;
        text += wchar_t(uint8_t(c));
    }
    return text.empty() ? L"Unique identity unavailable" : text;
}
bool has_stick(const ControllerSnapshot &controller, unsigned stick) {
    return controller.kind == ControllerKind::Pro ||
           (stick == 1) == (controller.kind == ControllerKind::JoyRight);
}
} // namespace
SettingsWindow::~SettingsWindow() {
    close();
}
int SettingsWindow::units(int value) const {
    return MulDiv(value, int(dpi_), 96);
}
void SettingsWindow::fonts() {
    for (auto font : {font_, heading_font_, control_font_})
        if (font)
            DeleteObject(font);
    // The canvas uses logical 96-DPI units; child controls use physical pixels.
    font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                        CLEARTYPE_QUALITY, 0, L"Segoe UI");
    heading_font_ = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                                0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    control_font_ = CreateFontW(-units(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    for (auto &panel : panels_)
        for (auto control : panel.controls)
            if (control)
                SendMessageW(control, WM_SETFONT, WPARAM(control_font_), TRUE);
}
void SettingsWindow::show(HWND owner, HICON icon) {
    if (window_) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        return;
    }
    close();
    WNDCLASSW cls{};
    cls.lpfnWndProc = procedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = icon;
    cls.lpszClassName = ClassName;
    RegisterClassW(&cls);
    dpi_ = GetDpiForWindow(owner);
    if (!dpi_)
        dpi_ = 96;
    brush_ = CreateSolidBrush(graphics::Card);
    fonts();
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = std::min(units(1120), int(monitor.rcWork.right - monitor.rcWork.left));
    const int height = std::min(units(930), int(monitor.rcWork.bottom - monitor.rcWork.top));
    const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    window_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, ClassName,
                              switch2::SettingsTitle,
                              WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN, x, y, width,
                              height, owner, nullptr, cls.hInstance, this);
    if (!window_) {
        close();
        return;
    }
    worker_ = std::jthread([this](std::stop_token stop) { worker(stop); });
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
}
void SettingsWindow::close() {
    if (window_)
        DestroyWindow(window_);
    if (worker_.joinable()) {
        worker_.request_stop();
        wake_.notify_all();
        worker_.join();
    }
    for (auto &font : {&font_, &heading_font_, &control_font_}) {
        if (*font)
            DeleteObject(*font);
        *font = nullptr;
    }
    if (brush_) {
        DeleteObject(brush_);
        brush_ = nullptr;
    }
    panels_ = {};
    current_ = {};
    scroll_ = 0;
    std::lock_guard lock(mutex_);
    pending_.clear();
    completed_.clear();
    received_ = posted_ = false;
}
void SettingsWindow::worker(std::stop_token stop) {
    for (;;) {
        Save operation;
        bool saving = false;
        {
            std::lock_guard lock(mutex_);
            // Finish edits already released by the user before closing the window.
            if (stop.stop_requested() && pending_.empty())
                break;
            if (!pending_.empty()) {
                operation = pending_.front();
                pending_.pop_front();
                saving = true;
            }
        }
        ipc::Request request;
        request.command = ipc::Command::SettingsRead;
        if (saving)
            request = operation.request;
        ipc::SettingsResponse response;
        const bool connected = ipc::exchange(request, response);
        {
            std::lock_guard lock(mutex_);
            if (saving) {
                operation.success = connected && !response.result;
                operation.error = connected ? response.result : ERROR_SERVICE_NOT_ACTIVE;
                completed_.push_back(operation);
            }
            if (connected)
                incoming_ = response;
            received_ = true;
            delivered_online_ = connected;
            if (!posted_) {
                posted_ = true;
                PostMessageW(window_, Updated, 0, 0);
            }
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(40),
                       [&] { return stop.stop_requested() || !pending_.empty(); });
    }
}
void SettingsWindow::receive() {
    std::deque<Save> results;
    ipc::SettingsResponse next;
    {
        std::lock_guard lock(mutex_);
        posted_ = false;
        if (!received_)
            return;
        next = incoming_;
        online_ = delivered_online_;
        received_ = false;
        results.swap(completed_);
    }
    const bool roster_changed = next.generation != current_.generation;
    current_ = next;
    if (roster_changed)
        rebuild();
    for (auto &result : results) {
        if (result.request.generation != current_.generation ||
            result.request.mode >= panels_.size())
            continue;
        auto &panel = panels_[result.request.mode];
        panel.saving = panel.editing = false;
        panel.status = result.success ? L"Saved to this controller"
                                      : L"Not saved. Please try again. (" +
                                            std::to_wstring(result.error) + L")";
        if (result.success && notify_)
            notify_(result.description);
    }
    const auto now = GetTickCount64();
    for (unsigned i = 0; i < 5; ++i) {
        auto &panel = panels_[i];
        const auto &controller = current_.controllers[i];
        if (!controller.connected)
            continue;
        if (!panel.editing && !panel.saving)
            panel.draft = controller.profile;
        const auto &motion = current_.motion[i];
        if (motion.sample_time_100ns != panel.last_sample) {
            panel.last_received = now;
            panel.last_sample = motion.sample_time_100ns;
        }
        if (now - panel.rate_time >= 1000) {
            panel.rate =
                panel.rate_time && motion.samples >= panel.rate_samples
                    ? (motion.samples - panel.rate_samples) * 1000.0 / (now - panel.rate_time)
                    : 0;
            panel.rate_samples = motion.samples;
            panel.rate_time = now;
        }
        const bool enabled =
            online_ && !panel.saving && !controller_profile_key(controller).empty();
        for (unsigned j = 0; j < panel.controls.size(); ++j)
            if (panel.controls[j]) {
                EnableWindow(panel.controls[j], j == 5 ? online_ : enabled);
                if (!panel.editing) {
                    if (j == 0 || j == 2)
                        SendMessageW(panel.controls[j], TBM_SETPOS, TRUE,
                                     panel.draft.deadzone[j / 2]);
                    if (j == 1 || j == 3)
                        SendMessageW(panel.controls[j], CB_SETCURSEL,
                                     (panel.draft.circular_mask >> (j / 2)) & 1, 0);
                    if (j == 4)
                        SendMessageW(panel.controls[j], TBM_SETPOS, TRUE,
                                     panel.draft.gyro_counts_per_degree);
                }
            }
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::rebuild() {
    for (auto &panel : panels_) {
        for (auto control : panel.controls)
            if (control)
                DestroyWindow(control);
        panel = {};
    }
    for (unsigned i = 0; i < 5; ++i) {
        const auto &c = current_.controllers[i];
        if (!c.connected)
            continue;
        auto &panel = panels_[i];
        panel.draft = c.profile;
        const auto make = [&](unsigned slot, const wchar_t *type, const wchar_t *text,
                              DWORD style) {
            auto control = CreateWindowExW(
                0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, window_,
                HMENU(INT_PTR(100 + i * 10 + slot)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, WPARAM(control_font_), TRUE);
            panel.controls[slot] = control;
            return control;
        };
        for (unsigned stick = 0; stick < 2; ++stick)
            if (has_stick(c, stick)) {
                auto slider =
                    make(stick * 2, TRACKBAR_CLASSW, L"Dead zone", TBS_HORZ | TBS_NOTICKS);
                SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 40));
                SendMessageW(slider, TBM_SETPAGESIZE, 0, 1);
                auto combo = make(stick * 2 + 1, WC_COMBOBOXW, L"Dead zone shape",
                                  CBS_DROPDOWNLIST | WS_VSCROLL);
                SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(L"Axis dead zone"));
                SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(L"Circular dead zone"));
            }
        if (c.kind != ControllerKind::Pro) {
            auto slider = make(4, TRACKBAR_CLASSW, L"Gyro mouse response", TBS_HORZ | TBS_NOTICKS);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 360));
            SendMessageW(slider, TBM_SETPAGESIZE, 0, 5);
        }
        make(5, WC_BUTTONW, L"Recenter heading", BS_PUSHBUTTON);
        make(6, WC_BUTTONW, L"Reset this controller", BS_PUSHBUTTON);
        panel.status = controller_profile_key(c).empty()
                           ? L"Identity unavailable - settings cannot be saved"
                           : L"Changes save automatically to this controller";
    }
    layout();
}
void SettingsWindow::layout() {
    if (!window_)
        return;
    RECT area;
    GetClientRect(window_, &area);
    const int width = MulDiv(area.right, 96, int(dpi_)),
              height = MulDiv(area.bottom, 96, int(dpi_));
    unsigned count = 0;
    for (const auto &c : current_.controllers)
        count += c.connected != 0;
    content_height_ = HeaderHeight + int(count) * PanelHeight + 20;
    scroll_ = std::clamp(scroll_, 0, std::max(0, content_height_ - height));
    SCROLLINFO info{
        sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, content_height_ - 1, UINT(height),
        scroll_};
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    int top = HeaderHeight - scroll_;
    for (unsigned i = 0; i < 5; ++i) {
        const auto &c = current_.controllers[i];
        if (!c.connected)
            continue;
        auto &panel = panels_[i];
        const int model_x = c.kind == ControllerKind::Pro ? 488 : 270;
        const auto place = [&](unsigned slot, int x, int y, int w, int h) {
            if (panel.controls[slot])
                MoveWindow(panel.controls[slot], units(x), units(top + y), units(w), units(h),
                           TRUE);
        };
        int column = 0;
        for (unsigned stick = 0; stick < 2; ++stick)
            if (has_stick(c, stick)) {
                const int x = 36 + column++ * 222;
                place(stick * 2, x, 293, 198, 28);
                place(stick * 2 + 1, x, 323, 198, 110);
            }
        place(5, model_x, 290, 170, 28);
        place(6, width - 275, 337, 238, 28);
        place(4, width - 275, 290, 238, 28);
        top += PanelHeight;
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::scroll(int position) {
    scroll_ = position;
    layout();
}
void SettingsWindow::save(unsigned index, const std::wstring &description) {
    auto &panel = panels_[index];
    if (panel.saving || !valid_profile(panel.draft))
        return;
    panel.saving = true;
    panel.editing = false;
    panel.status = L"Saving...";
    Save save;
    save.request.command = ipc::Command::SettingsSave;
    save.request.generation = current_.generation;
    save.request.mode = index;
    save.request.pair = ipc::pack_profile(panel.draft);
    save.description = name(current_.controllers[index]) + L" / Player " +
                       std::to_wstring(current_.controllers[index].player) + L" - " + description;
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(save);
    }
    wake_.notify_one();
}
void SettingsWindow::changed(HWND control, UINT action) {
    const int id = GetDlgCtrlID(control) - 100;
    if (id < 0 || id >= 50)
        return;
    const unsigned index = unsigned(id) / 10, slot = unsigned(id) % 10;
    auto &panel = panels_[index];
    if (!current_.controllers[index].connected || panel.saving)
        return;
    if (slot == 5) {
        panel.heading = graphics::yaw(current_.motion[index].orientation);
        return;
    }
    if (slot == 6) {
        panel.draft = {};
        save(index, L"Controller settings reset to defaults");
        return;
    }
    panel.editing = true;
    std::wstring text;
    if (slot == 0 || slot == 2) {
        const unsigned stick = slot / 2;
        panel.draft.deadzone[stick] = uint8_t(SendMessageW(control, TBM_GETPOS, 0, 0));
        text = (stick ? L"Right stick" : L"Left stick") + std::wstring(L" dead zone: ") +
               std::to_wstring(panel.draft.deadzone[stick]) + L"%";
    } else if (slot == 1 || slot == 3) {
        const unsigned stick = slot / 2;
        const bool circular = SendMessageW(control, CB_GETCURSEL, 0, 0) == 1;
        panel.draft.circular_mask =
            uint8_t((panel.draft.circular_mask & ~(1u << stick)) | (unsigned(circular) << stick));
        text = (stick ? L"Right stick" : L"Left stick") +
               std::wstring(circular ? L": Circular dead zone" : L": Axis dead zone");
    } else if (slot == 4) {
        panel.draft.gyro_counts_per_degree = uint16_t(SendMessageW(control, TBM_GETPOS, 0, 0));
        text = L"Gyro mouse response: " + std::to_wstring(panel.draft.gyro_counts_per_degree) +
               L" counts/degree (" + number(panel.draft.gyro_counts_per_degree * 100.0 / 90, 0) +
               L"%)";
    }
    if (action != TB_THUMBTRACK && action != TB_THUMBPOSITION)
        save(index, text);
    InvalidateRect(window_, nullptr, FALSE);
}
void SettingsWindow::paint(HDC dc) {
    RECT area;
    GetClientRect(window_, &area);
    const int width = MulDiv(area.right, 96, int(dpi_)),
              height = MulDiv(area.bottom, 96, int(dpi_));
    const auto memory = CreateCompatibleDC(dc);
    const auto bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
    const auto old_bitmap = SelectObject(memory, bitmap);
    SetGraphicsMode(memory, GM_ADVANCED);
    XFORM scale{dpi_ / 96.0f, 0, 0, dpi_ / 96.0f, 0, 0};
    SetWorldTransform(memory, &scale);
    graphics::fill(memory, {0, 0, width, height}, graphics::Background);
    SelectObject(memory, heading_font_);
    graphics::label(memory, {24, 16 - scroll_, width - 24, 44 - scroll_},
                    L"Controller settings & diagnostics");
    SelectObject(memory, font_);
    graphics::label(memory, {24, 47 - scroll_, width - 24, 70 - scroll_},
                    L"Individual profiles  /  Live sticks  /  Motion preview", graphics::Muted);
    graphics::label(
        memory, {24, 71 - scroll_, width - 24, 94 - scroll_},
        online_
            ? L"White dot: raw stick   /   Blue ring: gamepad output   /   Shading: ignored input"
            : L"Waiting for the controller manager...",
        online_ ? graphics::Muted : RGB(255, 186, 105));
    int top = HeaderHeight - scroll_;
    bool any = false;
    for (unsigned i = 0; i < 5; ++i) {
        auto c = current_.controllers[i];
        if (!c.connected)
            continue;
        any = true;
        auto &panel = panels_[i];
        c.profile = panel.draft;
        const auto &motion = current_.motion[i];
        if (top + PanelHeight > 0 && top < height) {
            graphics::fill(memory, {20, top, width - 20, top + PanelHeight - 14}, graphics::Card);
            SelectObject(memory, heading_font_);
            graphics::label(memory, {36, top + 10, width - 32, top + 36},
                            name(c) + L"  /  Player " + std::to_wstring(c.player));
            SelectObject(memory, font_);
            graphics::label(memory, {36, top + 37, width - 32, top + 59},
                            (c.transport == 1 ? L"USB" : L"Bluetooth") +
                                std::wstring(c.sideways ? L"  /  Sideways" : L"  /  Upright") +
                                L"  /  ID " + identity(c),
                            graphics::Muted);
            int column = 0;
            for (unsigned stick = 0; stick < 2; ++stick)
                if (has_stick(c, stick)) {
                    const int x = 36 + column++ * 222;
                    graphics::label(memory, {x, top + 62, x + 198, top + 84},
                                    stick ? L"RIGHT STICK" : L"LEFT STICK", graphics::Muted);
                    graphics::stick(memory, {x, top + 86, x + 198, top + 264}, c, stick);
                    graphics::label(memory, {x, top + 266, x + 198, top + 290},
                                    L"Dead zone " + std::to_wstring(c.profile.deadzone[stick]) +
                                        L"%  /  X " + number(c.raw_sticks[stick * 2] / 32767.0, 2) +
                                        L"  Y " + number(c.raw_sticks[stick * 2 + 1] / 32767.0, 2),
                                    graphics::Muted);
                }
            const int model_x = c.kind == ControllerKind::Pro ? 488 : 270, metrics = width - 275;
            graphics::label(memory, {model_x, top + 62, metrics - 20, top + 84},
                            L"RELATIVE ORIENTATION", graphics::Muted);
            graphics::controller(memory, {model_x, top + 86, metrics - 24, top + 280}, c.kind,
                                 motion.orientation, panel.heading,
                                 model_button_state(c, uint32_t(GetTickCount64()), online_));
            graphics::label(
                memory, {model_x, top + 325, metrics - 20, top + 370},
                L"Rotation only. Heading may drift.\nRecenter does not change mouse aiming.",
                graphics::Muted, DT_LEFT | DT_WORDBREAK);
            const bool fresh =
                online_ && motion.ready && GetTickCount64() - panel.last_received < 500;
            graphics::label(memory, {metrics, top + 62, width - 36, top + 84},
                            fresh ? L"IMU LIVE  /  " + number(panel.rate) + L" Hz"
                                  : L"IMU waiting for samples",
                            fresh ? graphics::Accent : graphics::Muted);
            graphics::label(memory, {metrics, top + 90, width - 36, top + 113},
                            L"Angular velocity  (degrees/s)", graphics::Muted);
            const auto axes = [](const auto &v) {
                return L"X " + number(v[0], 2) + L"   Y " + number(v[1], 2) + L"   Z " +
                       number(v[2], 2);
            };
            graphics::label(memory, {metrics, top + 115, width - 36, top + 137}, axes(motion.gyro));
            graphics::label(memory, {metrics, top + 143, width - 36, top + 165},
                            L"Acceleration, including gravity  (g)", graphics::Muted);
            graphics::label(memory, {metrics, top + 167, width - 36, top + 189},
                            axes(motion.acceleration));
            const auto &q = motion.orientation;
            const double roll =
                std::atan2(2 * (q[0] * q[1] + q[2] * q[3]), 1 - 2 * (q[1] * q[1] + q[2] * q[2])) *
                180 / std::numbers::pi;
            const double pitch =
                std::asin(std::clamp(2.0 * (q[0] * q[2] - q[3] * q[1]), -1.0, 1.0)) * 180 /
                std::numbers::pi;
            graphics::label(
                memory, {metrics, top + 196, width - 36, top + 218},
                L"Roll " + number(roll) + L"  Pitch " + number(pitch) + L"  Yaw " +
                    number(std::remainder(graphics::yaw(q) - panel.heading, 2 * std::numbers::pi) *
                           180 / std::numbers::pi),
                graphics::Muted);
            graphics::label(memory, {metrics, top + 222, width - 36, top + 244},
                            L"Samples " + std::to_wstring(motion.samples), graphics::Muted);
            if (c.kind != ControllerKind::Pro) {
                graphics::label(
                    memory, {metrics, top + 261, width - 36, top + 287},
                    L"Gyro mouse: " + std::to_wstring(c.profile.gyro_counts_per_degree) +
                        L" counts/degree (" +
                        number(c.profile.gyro_counts_per_degree * 100.0 / 90, 0) + L"%)");
                graphics::label(memory, {metrics, top + 316, width - 36, top + 335},
                                L"Higher = faster. Default: 90.", graphics::Muted);
            } else
                graphics::label(memory, {metrics, top + 265, width - 36, top + 310},
                                L"Gyro mouse settings are available\nfor Joy-Con controllers.",
                                graphics::Muted, DT_LEFT | DT_WORDBREAK);
            graphics::label(memory, {36, top + 354, metrics - 24, top + 378}, panel.status,
                            graphics::Muted);
        }
        top += PanelHeight;
    }
    if (!any)
        graphics::label(memory, {24, 130 - scroll_, width - 24, 190 - scroll_},
                        L"Connect a paired controller to see its sticks and motion.",
                        graphics::Muted);
    ModifyWorldTransform(memory, nullptr, MWT_IDENTITY);
    BitBlt(dc, 0, 0, area.right, area.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}
LRESULT CALLBACK SettingsWindow::procedure(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam) {
    auto *self = reinterpret_cast<SettingsWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SettingsWindow *>(
            reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(self));
    }
    return self ? self->message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}
LRESULT SettingsWindow::message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case Updated:
        receive();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(window_, &ps);
        paint(dc);
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        fonts();
        const auto rect = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO *>(lparam);
        info->ptMinTrackSize = {units(1080), units(500)};
        return 0;
    }
    case WM_HSCROLL:
        changed(HWND(lparam), LOWORD(wparam));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL && !lparam) {
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return 0;
        }
        if (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == BN_CLICKED)
            changed(HWND(lparam), TB_ENDTRACK);
        return 0;
    case WM_MOUSEWHEEL:
        scroll(scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * 60 / WHEEL_DELTA);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window_, SB_VERT, &info);
        int pos = scroll_;
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            pos -= 32;
            break;
        case SB_LINEDOWN:
            pos += 32;
            break;
        case SB_PAGEUP:
            pos -= int(info.nPage);
            break;
        case SB_PAGEDOWN:
            pos += int(info.nPage);
            break;
        case SB_THUMBTRACK:
            pos = info.nTrackPos;
            break;
        case SB_TOP:
            pos = 0;
            break;
        case SB_BOTTOM:
            pos = content_height_;
            break;
        }
        scroll(pos);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        SetTextColor(HDC(wparam), graphics::Text);
        SetBkColor(HDC(wparam), graphics::Card);
        return LRESULT(brush_);
    case WM_CLOSE:
        ShowWindow(window_, SW_HIDE);
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        if (worker_.joinable()) {
            worker_.request_stop();
            wake_.notify_all();
            worker_.join();
        }
        return 0;
    case WM_NCDESTROY: {
        const auto result = DefWindowProcW(window_, message, wparam, lparam);
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        window_ = nullptr;
        return result;
    }
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}
} // namespace switch2
