#pragma once
#include "tray_events.hpp"
#include <windows.h>
#include <shellapi.h>

namespace switch2::tray {
class Caption {
  public:
    ~Caption();
    void show(HWND tray_window, UINT icon_id, HICON icon, const Notification &message, UINT closed_message);
    bool visible() const { return window_ && IsWindowVisible(window_); }
    void close();

  private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void paint();
    void animate();
    int measure(const std::wstring &text, HFONT font, int width) const;
    HWND window_{}, tray_window_{};
    UINT closed_message_{};
    HICON icon_{};
    HFONT header_font_{}, title_font_{}, body_font_{};
    Notification message_;
    RECT anchor_{}, title_rect_{}, body_rect_{};
    bool have_anchor_{};
    int width_{}, height_{}, scale_{96};
    uint64_t started_{};
};
} // namespace switch2::tray
