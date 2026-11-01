#include "settings_graphics.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <iostream>
#include <vector>

// Off-screen rendering check: exercises the actual model renderer without
// injecting input or opening a desktop window.
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::cerr << "Usage: SettingsRenderCheck output.png\n";
        return 2;
    }
    constexpr int width = 1280, height = 870, column = 320, row = 290;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void *pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc)
        return 1;
    const auto old = SelectObject(dc, bitmap);
    const auto font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                                  0, DEFAULT_QUALITY, 0, L"Segoe UI");
    const auto old_font = SelectObject(dc, font);
    using namespace switch2;
    namespace graphics = settings_graphics;
    graphics::fill(dc, {0, 0, width, height}, graphics::Card);
    const ControllerKind kinds[]{ControllerKind::Pro, ControllerKind::JoyLeft,
                                 ControllerKind::JoyRight};
    const wchar_t *names[]{L"Pro Controller", L"Joy-Con (L)", L"Joy-Con (R)"};
    const wchar_t *views[]{L"Released", L"Pressed / front", L"Pressed / back", L"Pressed / edge"};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t mask = 0;
        for (const auto &button : model_buttons(kinds[i]))
            mask |= button.button;
        for (int view = 0; view < 4; ++view) {
            const int x = view * column, y = int(i) * row;
            std::array<float, 4> rotation{1, 0, 0, 0};
            if (view == 2)
                rotation = {0, 1, 0, 0};
            if (view == 3)
                rotation = i == 0 ? std::array<float, 4>{.70710678f, .70710678f, 0, 0}
                                  : std::array<float, 4>{.70710678f, 0,
                                                         i == 1 ? -.70710678f : .70710678f, 0};
            graphics::label(dc, {x + 10, y + 5, x + column - 10, y + 30},
                            std::wstring(names[i]) + L" / " + views[view]);
            graphics::controller(dc, {x + 8, y + 35, x + column - 8, y + row - 8}, kinds[i],
                                 rotation, 0, {view ? mask : 0, view ? mask : 0, true});
        }
    }
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token{};
    bool saved = false;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) == Gdiplus::Ok) {
        UINT count = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto *encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(storage.data());
        Gdiplus::GetImageEncoders(count, bytes, encoders);
        {
            Gdiplus::Bitmap rendered(bitmap, nullptr);
            for (UINT i = 0; i < count; ++i)
                if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
                    saved = rendered.Save(argv[1], &encoders[i].Clsid, nullptr) == Gdiplus::Ok;
        }
        Gdiplus::GdiplusShutdown(token);
    }
    SelectObject(dc, old_font);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return saved ? 0 : 1;
}
