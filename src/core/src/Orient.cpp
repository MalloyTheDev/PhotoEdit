#include "pe/core/Orient.hpp"

#include <algorithm>

namespace pe {

Size orientedCanvas(Orient op, Size canvas) noexcept {
    switch (op) {
        case Orient::Rotate90CW:
        case Orient::Rotate90CCW:
            return Size{canvas.height, canvas.width};
        case Orient::FlipHorizontal:
        case Orient::FlipVertical:
        case Orient::Rotate180:
            break;
    }
    return canvas;
}

Point orientForward(Orient op, Size canvas, Point s) noexcept {
    const int w = canvas.width;
    const int h = canvas.height;
    switch (op) {
        case Orient::FlipHorizontal:
            return Point{w - 1 - s.x, s.y};
        case Orient::FlipVertical:
            return Point{s.x, h - 1 - s.y};
        case Orient::Rotate180:
            return Point{w - 1 - s.x, h - 1 - s.y};
        case Orient::Rotate90CW:
            return Point{h - 1 - s.y, s.x};
        case Orient::Rotate90CCW:
            return Point{s.y, w - 1 - s.x};
    }
    return s;
}

Point orientInverse(Orient op, Size canvas, Point d) noexcept {
    const int w = canvas.width;
    const int h = canvas.height;
    switch (op) {
        case Orient::FlipHorizontal:
            return Point{w - 1 - d.x, d.y};  // self-inverse
        case Orient::FlipVertical:
            return Point{d.x, h - 1 - d.y};  // self-inverse
        case Orient::Rotate180:
            return Point{w - 1 - d.x, h - 1 - d.y};  // self-inverse
        case Orient::Rotate90CW:
            return Point{d.y, h - 1 - d.x};
        case Orient::Rotate90CCW:
            return Point{w - 1 - d.y, d.x};
    }
    return d;
}

Rect orientRect(Orient op, Size canvas, Rect r) noexcept {
    if (r.isEmpty()) return Rect{};
    const Point a = orientForward(op, canvas, Point{r.left(), r.top()});
    const Point b = orientForward(op, canvas, Point{r.right() - 1, r.bottom() - 1});
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    return Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

}  // namespace pe
