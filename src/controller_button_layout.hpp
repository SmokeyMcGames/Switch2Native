#pragma once
#include "controller_settings.hpp"
#include <span>

namespace switch2 {
enum class ButtonSurface { Front, Back, Top, LeftSide, RightSide };
struct ModelButton {
    Button button;
    const wchar_t *name;
    double x, y, z, width, height;
    ButtonSurface surface{ButtonSurface::Front};
};
inline constexpr ModelButton ProModelButtons[]{
    {A, L"A", 1.20, .46, .25, .095, .095},
    {B, L"B", .98, .24, .25, .095, .095},
    {X, L"X", .98, .68, .25, .095, .095},
    {Y, L"Y", .76, .46, .25, .095, .095},
    {Up, L"Up", -.55, -.06, .25, .09, .09},
    {Down, L"Down", -.55, -.38, .25, .09, .09},
    {Left, L"Left", -.71, -.22, .25, .09, .09},
    {Right, L"Right", -.39, -.22, .25, .09, .09},
    {L3, L"L3", -.9, .45, .25, .25, .25},
    {R3, L"R3", .55, -.22, .25, .25, .25},
    {Minus, L"Minus", -.48, .73, .25, .10, .055},
    {Plus, L"Plus", .48, .73, .25, .09, .09},
    {Capture, L"Capture", -.24, .38, .25, .09, .09},
    {Home, L"Home", .24, .38, .25, .09, .09},
    {Chat, L"C", 0, -.18, .25, .10, .10},
    {L, L"L", -.94, 1.025, .12, .28, .08, ButtonSurface::Top},
    {R, L"R", .94, 1.025, .12, .28, .08, ButtonSurface::Top},
    {ZL, L"ZL", -.94, 1.025, -.12, .28, .08, ButtonSurface::Top},
    {ZR, L"ZR", .94, 1.025, -.12, .28, .08, ButtonSurface::Top},
    {GL, L"GL", -.9, -.26, -.25, .16, .26, ButtonSurface::Back},
    {GR, L"GR", .9, -.26, -.25, .16, .26, ButtonSurface::Back}};
inline constexpr ModelButton LeftModelButtons[]{
    {Up, L"Up", 0, -.01, .25, .10, .10},
    {Down, L"Down", 0, -.49, .25, .10, .10},
    {Left, L"Left", -.24, -.25, .25, .10, .10},
    {Right, L"Right", .24, -.25, .25, .10, .10},
    {L3, L"L3", 0, .65, .25, .26, .26},
    {Minus, L"Minus", .3, 1.07, .25, .10, .045},
    {Capture, L"Capture", -.25, -.87, .25, .09, .09},
    {L, L"L", 0, 1.38, .11, .36, .08, ButtonSurface::Top},
    {ZL, L"ZL", 0, 1.38, -.12, .36, .08, ButtonSurface::Top},
    {SLLeft, L"SL", .64, .55, 0, .19, .14, ButtonSurface::RightSide},
    {SRLeft, L"SR", .64, -.55, 0, .19, .14, ButtonSurface::RightSide}};
inline constexpr ModelButton RightModelButtons[]{
    {A, L"A", .24, .65, .25, .10, .10},
    {B, L"B", 0, .41, .25, .10, .10},
    {X, L"X", 0, .89, .25, .10, .10},
    {Y, L"Y", -.24, .65, .25, .10, .10},
    {R3, L"R3", 0, -.25, .25, .26, .26},
    {Plus, L"Plus", -.3, 1.07, .25, .065, .065},
    {Home, L"Home", .25, -.87, .25, .09, .09},
    {Chat, L"C", .25, -1.14, .25, .09, .09},
    {R, L"R", 0, 1.38, .11, .36, .08, ButtonSurface::Top},
    {ZR, L"ZR", 0, 1.38, -.12, .36, .08, ButtonSurface::Top},
    {SRRight, L"SR", -.64, .55, 0, .19, .14, ButtonSurface::LeftSide},
    {SLRight, L"SL", -.64, -.55, 0, .19, .14, ButtonSurface::LeftSide}};
inline std::span<const ModelButton> model_buttons(ControllerKind kind) {
    switch (kind) {
    case ControllerKind::Pro:
        return ProModelButtons;
    case ControllerKind::JoyLeft:
        return LeftModelButtons;
    case ControllerKind::JoyRight:
        return RightModelButtons;
    }
    return {};
}
struct ModelButtonState {
    uint32_t held{}, lit{};
    bool available{};
};
inline ModelButtonState model_button_state(const ControllerSnapshot &snapshot, uint32_t now_ms,
                                           bool online) {
    if (!online || !snapshot.connected || !(snapshot.diagnostic_flags & 1) ||
        uint32_t(now_ms - snapshot.input_tick_ms) > 500)
        return {};
    uint32_t supported = 0;
    for (const auto &button : model_buttons(snapshot.kind))
        supported |= button.button;
    return {snapshot.buttons & supported, (snapshot.buttons | snapshot.recent_buttons) & supported,
            true};
}
inline std::wstring model_button_names(ControllerKind kind, uint32_t buttons) {
    std::wstring names;
    for (const auto &button : model_buttons(kind))
        if (buttons & button.button) {
            if (!names.empty())
                names += L" + ";
            names += button.name;
        }
    return names;
}
} // namespace switch2
