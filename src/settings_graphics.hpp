#pragma once
#include "controller_settings.hpp"
#include "controller_button_layout.hpp"
#include <windows.h>

namespace switch2::settings_graphics {
inline constexpr COLORREF Background = RGB(17, 22, 30), Card = RGB(25, 32, 43),
                          Grid = RGB(47, 59, 75), Muted = RGB(154, 171, 193),
                          Text = RGB(235, 241, 249), Accent = RGB(90, 196, 255),
                          Pressed = RGB(91, 235, 158);
void fill(HDC dc, RECT rect, COLORREF color);
void label(HDC dc, RECT rect, const std::wstring &text, COLORREF color = Text,
           UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER);
void stick(HDC dc, RECT rect, const ControllerSnapshot &controller, unsigned stick);
void controller(HDC dc, RECT rect, ControllerKind kind, std::array<float, 4> orientation,
                double heading, ModelButtonState buttons);
double yaw(const std::array<float, 4> &orientation);
} // namespace switch2::settings_graphics
