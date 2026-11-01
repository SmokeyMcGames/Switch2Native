#include "settings_graphics.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace switch2::settings_graphics {
void fill(HDC dc, RECT rect, COLORREF color) {
    const auto brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
void label(HDC dc, RECT rect, const std::wstring &text, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), int(text.size()), &rect, format | DT_NOPREFIX);
}
namespace {
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    const auto pen = CreatePen(PS_SOLID, width, color);
    const auto old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}
void ellipse(HDC dc, RECT rect, COLORREF border, COLORREF interior, bool hollow = false) {
    const auto pen = CreatePen(PS_SOLID, 1, border);
    const auto brush = hollow ? HBRUSH(GetStockObject(NULL_BRUSH)) : CreateSolidBrush(interior);
    const auto old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, brush);
    Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    if (!hollow)
        DeleteObject(brush);
}
using Vector = std::array<double, 3>;
struct Face {
    std::vector<Vector> vertices;
    COLORREF color;
    bool surface_detail{};
    Vector normal{};
};
Vector rotate(Vector v, const std::array<float, 4> &q) {
    const auto [w, x, y, z] = q;
    const Vector t{2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])};
    return {v[0] + w * t[0] + y * t[2] - z * t[1], v[1] + w * t[1] + z * t[0] - x * t[2],
            v[2] + w * t[2] + x * t[1] - y * t[0]};
}
void body(std::vector<Face> &faces, const std::vector<std::array<double, 2>> &outline,
          COLORREF color) {
    Face top{{}, color}, bottom{{}, RGB(33, 39, 50)};
    for (auto p : outline) {
        top.vertices.push_back({p[0], p[1], 0.22});
        bottom.vertices.push_back({p[0], p[1], -0.22});
    }
    faces.push_back(top);
    faces.push_back(bottom);
    for (size_t i = 0; i < outline.size(); ++i) {
        auto a = outline[i], b = outline[(i + 1) % outline.size()];
        faces.push_back(
            {{{a[0], a[1], -.22}, {b[0], b[1], -.22}, {b[0], b[1], .22}, {a[0], a[1], .22}},
             RGB(54, 66, 83)});
    }
}
void button(std::vector<Face> &faces, const ModelButton &button, bool pressed) {
    Vector horizontal{1, 0, 0}, vertical{0, 1, 0}, normal{0, 0, 1};
    switch (button.surface) {
    case ButtonSurface::Front:
        break;
    case ButtonSurface::Back:
        vertical = {0, -1, 0};
        normal = {0, 0, -1};
        break;
    case ButtonSurface::Top:
        vertical = {0, 0, -1};
        normal = {0, 1, 0};
        break;
    case ButtonSurface::LeftSide:
        horizontal = {0, -1, 0};
        vertical = {0, 0, 1};
        normal = {-1, 0, 0};
        break;
    case ButtonSurface::RightSide:
        horizontal = {0, 1, 0};
        vertical = {0, 0, 1};
        normal = {1, 0, 0};
        break;
    }
    Face face{{}, pressed ? Pressed : RGB(24, 32, 42), true, normal};
    for (int i = 0; i < 20; ++i) {
        const auto angle = i * std::numbers::pi / 10;
        Vector point{button.x, button.y, button.z};
        for (unsigned axis = 0; axis < 3; ++axis)
            point[axis] += horizontal[axis] * button.width * std::cos(angle) +
                           vertical[axis] * button.height * std::sin(angle);
        face.vertices.push_back(point);
    }
    faces.push_back(face);
}
} // namespace
void stick(HDC dc, RECT rect, const ControllerSnapshot &c, unsigned index) {
    fill(dc, rect, Background);
    const int cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
    const int radius = std::min(rect.right - rect.left, rect.bottom - rect.top) / 2 - 10;
    const int dz = int(std::lround(radius * c.profile.deadzone[index] / 100.0));
    const RECT dead{cx - dz, cy - dz, cx + dz + 1, cy + dz + 1};
    if (c.profile.circular_mask & (1 << index))
        ellipse(dc, dead, RGB(107, 127, 150), RGB(55, 65, 80));
    else
        fill(dc, dead, RGB(55, 65, 80));
    for (int i = -4; i <= 4; ++i) {
        const int offset = i * radius / 4;
        line(dc, cx + offset, cy - radius, cx + offset, cy + radius, Grid);
        line(dc, cx - radius, cy + offset, cx + radius, cy + offset, Grid);
    }
    ellipse(dc, {cx - radius, cy - radius, cx + radius + 1, cy + radius + 1}, Muted, 0, true);
    line(dc, cx - radius, cy, cx + radius, cy, RGB(92, 106, 124));
    line(dc, cx, cy - radius, cx, cy + radius, RGB(92, 106, 124));
    if (!(c.profile.circular_mask & (1 << index))) {
        line(dc, dead.left, dead.top, dead.right, dead.top, Muted);
        line(dc, dead.right, dead.top, dead.right, dead.bottom, Muted);
        line(dc, dead.right, dead.bottom, dead.left, dead.bottom, Muted);
        line(dc, dead.left, dead.bottom, dead.left, dead.top, Muted);
    }
    const auto dot = [&](const auto &values, int size, COLORREF color, bool hollow) {
        const int x = cx + int(std::lround(values[index * 2] * radius / 32767.0));
        const int y = cy - int(std::lround(values[index * 2 + 1] * radius / 32767.0));
        ellipse(dc, {x - size, y - size, x + size + 1, y + size + 1}, color, color, hollow);
    };
    dot(c.output_sticks, 7, Accent, true);
    dot(c.raw_sticks, 3, Text, false);
}
double yaw(const std::array<float, 4> &q) {
    return std::atan2(2 * (q[0] * q[3] + q[1] * q[2]), 1 - 2 * (q[2] * q[2] + q[3] * q[3]));
}
void controller(HDC dc, RECT rect, ControllerKind kind, std::array<float, 4> q, double heading,
                ModelButtonState buttons) {
    fill(dc, rect, Background);
    std::vector<Face> faces;
    if (kind == ControllerKind::Pro) {
        body(faces,
             {{-1.6, -.8},
              {-1.45, .75},
              {-1.1, 1},
              {1.1, 1},
              {1.45, .75},
              {1.6, -.8},
              {1.1, -1.1},
              {.6, -.45},
              {-.6, -.45},
              {-1.1, -1.1}},
             RGB(88, 103, 124));
    } else {
        const bool right = kind == ControllerKind::JoyRight;
        body(faces,
             {{-.48, -1.35},
              {-.62, -1.15},
              {-.62, 1.18},
              {-.45, 1.36},
              {.45, 1.36},
              {.62, 1.18},
              {.62, -1.15},
              {.48, -1.35}},
             right ? RGB(205, 83, 79) : RGB(49, 149, 192));
    }
    for (const auto &control : model_buttons(kind))
        button(faces, control, (buttons.lit & control.button) != 0);
    // Recenter only world yaw. Pitch and roll remain gravity-referenced.
    const auto transform = [&](Vector v) {
        v = rotate(v, q);
        const double x = v[0] * std::cos(heading) + v[1] * std::sin(heading);
        const double y = -v[0] * std::sin(heading) + v[1] * std::cos(heading);
        return Vector{x, .78 * y + .63 * v[2], -.63 * y + .78 * v[2]};
    };
    std::erase_if(faces, [&](const auto &face) {
        return face.surface_detail && transform(face.normal)[2] <= 0;
    });
    for (auto &face : faces)
        for (auto &v : face.vertices)
            v = transform(v);
    const auto depth = [](const Face &f) {
        double sum = 0;
        for (auto v : f.vertices)
            sum += v[2];
        return sum / f.vertices.size();
    };
    // Draw visible surface details after their body faces so a large face's
    // average depth cannot incorrectly hide buttons attached to that surface.
    std::stable_sort(faces.begin(), faces.end(), [&](const auto &a, const auto &b) {
        if (a.surface_detail != b.surface_detail)
            return !a.surface_detail;
        return depth(a) < depth(b);
    });
    const double scale = std::min((rect.right - rect.left) / 4.4, (rect.bottom - rect.top) / 3.7);
    const double cx = (rect.left + rect.right) / 2.0, cy = (rect.top + rect.bottom) / 2.0;
    for (const auto &face : faces) {
        std::vector<POINT> points;
        for (auto v : face.vertices) {
            const double perspective = 6 / (6 - v[2]);
            points.push_back(
                {LONG(cx + v[0] * scale * perspective), LONG(cy - v[1] * scale * perspective)});
        }
        const auto brush = CreateSolidBrush(face.color),
                   old_brush = HBRUSH(SelectObject(dc, brush));
        const auto pen = CreatePen(PS_SOLID, 1, RGB(15, 21, 30)),
                   old_pen = HPEN(SelectObject(dc, pen));
        Polygon(dc, points.data(), int(points.size()));
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
    const auto caption = !buttons.available ? L"Waiting for button data"
                         : buttons.held     ? L"Held: " + model_button_names(kind, buttons.held)
                         : buttons.lit      ? L"Tap: " + model_button_names(kind, buttons.lit)
                                            : std::wstring(L"Green = pressed / brief taps flash");
    label(dc, {rect.left + 8, rect.bottom - 24, rect.right - 8, rect.bottom - 2}, caption,
          buttons.lit ? Pressed : Muted, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}
} // namespace switch2::settings_graphics
