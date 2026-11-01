#include "tray_caption.hpp"
#include <algorithm>

namespace switch2::tray {
namespace {
constexpr wchar_t CaptionClass[] = L"Switch2Native.TrayCaption";
constexpr uint64_t FadeIn = 160, Hold = 3200, FadeOut = 240;
}
Caption::~Caption() {
    close();
    for (auto font : {header_font_, title_font_, body_font_}) if (font) DeleteObject(font);
}
void Caption::close() {
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
}
int Caption::measure(const std::wstring &text, HFONT font, int width) const {
    auto dc = GetDC(window_);
    auto previous = SelectObject(dc, font);
    RECT bounds{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
    return bounds.bottom;
}
void Caption::show(HWND tray_window, UINT icon_id, HICON icon, const Notification &message, UINT closed_message) {
    tray_window_ = tray_window;
    closed_message_ = closed_message;
    icon_ = icon;
    message_ = message;
    if (!window_) {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = window_proc;
        type.lpszClassName = CaptionClass;
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        // Status captions must never take focus or intercept mouse/game input.
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                     WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                 CaptionClass, L"Switch2Native Controller Driver", WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, type.hInstance, this);
        if (!window_) return;
    }
    const auto dpi = int(GetDpiForWindow(tray_window));
    if (!header_font_ || dpi != scale_) {
        scale_ = dpi ? dpi : 96;
        for (auto font : {header_font_, title_font_, body_font_}) if (font) DeleteObject(font);
        const auto font = [&](int size, int weight) {
            return CreateFontW(-MulDiv(size, scale_, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        header_font_ = font(12, FW_NORMAL);
        title_font_ = font(18, FW_SEMIBOLD);
        body_font_ = font(14, FW_NORMAL);
    }
    const auto px = [&](int value) { return MulDiv(value, scale_, 96); };
    NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
    identifier.hWnd = tray_window;
    identifier.uID = icon_id;
    RECT found{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &found))) {
        anchor_ = found;
        have_anchor_ = true;
    }
    MONITORINFO monitor{sizeof(monitor)};
    const auto display = have_anchor_ ? MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST)
                                      : MonitorFromWindow(tray_window, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(display, &monitor)) return;
    const auto &work = monitor.rcWork;
    width_ = std::min(px(360), int(work.right - work.left) - px(16));
    const int margin = px(14), text_width = width_ - margin * 2;
    title_rect_ = {margin, px(44), width_ - margin, 0};
    title_rect_.bottom = title_rect_.top + measure(message_.title, title_font_, text_width);
    body_rect_ = {margin, title_rect_.bottom + px(7), width_ - margin, 0};
    body_rect_.bottom = body_rect_.top + measure(message_.body, body_font_, text_width);
    height_ = std::min(int(body_rect_.bottom) + margin, int(work.bottom - work.top) - px(16));
    body_rect_.bottom = height_ - margin;
    int x = have_anchor_ ? (anchor_.left + anchor_.right - width_) / 2 : work.right - width_ - px(12);
    int y = work.bottom - height_ - px(12);
    if (have_anchor_) y = anchor_.top > (work.top + work.bottom) / 2
                             ? anchor_.top - height_ - px(8) : anchor_.bottom + px(8);
    x = std::clamp(x, int(work.left) + px(8), int(work.right) - width_ - px(8));
    y = std::clamp(y, int(work.top) + px(8), int(work.bottom) - height_ - px(8));
    const bool showing = visible();
    if (!showing) SetLayeredWindowAttributes(window_, 0, 0, LWA_ALPHA);
    SetWindowPos(window_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);
    const auto region = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, px(12), px(12));
    if (region && !SetWindowRgn(window_, region, FALSE)) DeleteObject(region);
    InvalidateRect(window_, nullptr, FALSE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    // Prepare the new content before starting the fade, including when a hidden
    // caption is reused during controller teardown.
    UpdateWindow(window_);
    // Rapid adjustments update the same caption rather than stacking windows.
    started_ = GetTickCount64() - (showing ? FadeIn : 0);
    SetTimer(window_, 1, 16, nullptr);
    animate();
}
void Caption::paint() {
    PAINTSTRUCT paint{};
    auto output = BeginPaint(window_, &paint);
    auto dc = CreateCompatibleDC(output);
    auto bitmap = CreateCompatibleBitmap(output, width_, height_);
    auto old_bitmap = SelectObject(dc, bitmap);
    const auto px = [&](int value) { return MulDiv(value, scale_, 96); };
    auto background = CreateSolidBrush(RGB(32, 33, 36));
    auto border = CreatePen(PS_SOLID, 1, RGB(81, 84, 91));
    auto old_brush = SelectObject(dc, background);
    auto old_pen = SelectObject(dc, border);
    RoundRect(dc, 0, 0, width_, height_, px(12), px(12));
    SetBkMode(dc, TRANSPARENT);
    DrawIconEx(dc, px(14), px(12), icon_, px(22), px(22), 0, nullptr, DI_NORMAL);
    auto old_font = SelectObject(dc, header_font_);
    SetTextColor(dc, RGB(185, 189, 198));
    RECT header{px(44), px(12), width_ - px(14), px(34)};
    DrawTextW(dc, L"Switch2Native Controller Driver", -1, &header, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, title_font_);
    SetTextColor(dc, RGB(245, 246, 248));
    DrawTextW(dc, message_.title.c_str(), -1, &title_rect_, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, body_font_);
    SetTextColor(dc, RGB(211, 214, 220));
    DrawTextW(dc, message_.body.c_str(), -1, &body_rect_, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    BitBlt(output, 0, 0, width_, height_, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font); SelectObject(dc, old_pen); SelectObject(dc, old_brush);
    SelectObject(dc, old_bitmap);
    DeleteObject(border); DeleteObject(background); DeleteObject(bitmap); DeleteDC(dc);
    EndPaint(window_, &paint);
}
void Caption::animate() {
    const auto elapsed = GetTickCount64() - started_;
    if (elapsed >= FadeIn + Hold + FadeOut) {
        KillTimer(window_, 1);
        ShowWindow(window_, SW_HIDE);
        PostMessageW(tray_window_, closed_message_, 0, 0);
        return;
    }
    const auto alpha = elapsed < FadeIn ? elapsed * 255 / FadeIn :
                       elapsed < FadeIn + Hold ? 255 : (FadeIn + Hold + FadeOut - elapsed) * 255 / FadeOut;
    SetLayeredWindowAttributes(window_, 0, BYTE(alpha), LWA_ALPHA);
}
LRESULT CALLBACK Caption::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<Caption *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Caption *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        if (message == WM_PAINT) { self->paint(); return 0; }
        if (message == WM_TIMER) { self->animate(); return 0; }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_NCDESTROY) self->window_ = nullptr;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace switch2::tray
