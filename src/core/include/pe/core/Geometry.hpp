#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pe {

// Integer point in document/pixel space. Sub-pixel positions (brush sampling,
// transforms) use floating-point types defined in the math module; this is for
// pixel-addressable coordinates.
struct Point {
    int x = 0;
    int y = 0;

    constexpr bool operator==(const Point&) const = default;
};

struct Size {
    int width = 0;
    int height = 0;

    constexpr bool operator==(const Size&) const = default;
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr int64_t area() const noexcept {
        return static_cast<int64_t>(width) * static_cast<int64_t>(height);
    }
};

// Half-open integer rectangle: covers x in [x, x+width), y in [y, y+height).
// This is the workhorse type for dirty-region tracking and tile math.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr bool operator==(const Rect&) const = default;

    [[nodiscard]] constexpr int left() const noexcept { return x; }
    [[nodiscard]] constexpr int top() const noexcept { return y; }
    [[nodiscard]] constexpr int right() const noexcept { return x + width; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + height; }

    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }

    [[nodiscard]] constexpr bool contains(Point p) const noexcept {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    // Smallest rect containing both. An empty rect is treated as "nothing".
    [[nodiscard]] constexpr Rect united(const Rect& o) const noexcept {
        if (isEmpty()) return o;
        if (o.isEmpty()) return *this;
        const int l = std::min(x, o.x);
        const int t = std::min(y, o.y);
        const int r = std::max(right(), o.right());
        const int b = std::max(bottom(), o.bottom());
        return Rect{l, t, r - l, b - t};
    }

    // Overlap of the two rects, or an empty rect if they do not touch.
    [[nodiscard]] constexpr Rect intersected(const Rect& o) const noexcept {
        const int l = std::max(x, o.x);
        const int t = std::max(y, o.y);
        const int r = std::min(right(), o.right());
        const int b = std::min(bottom(), o.bottom());
        if (r <= l || b <= t) return Rect{};
        return Rect{l, t, r - l, b - t};
    }

    [[nodiscard]] constexpr bool intersects(const Rect& o) const noexcept {
        return !intersected(o).isEmpty();
    }
};

// A 2x3 affine transform in document space (doubles for sub-pixel precision), mapping
// (x, y) -> (m00*x + m01*y + m02, m10*x + m11*y + m12). Used by the Transform tool to build a
// scale/rotate/translate and by transformLayerContent to resample a layer's pixels. Row-major.
struct Affine2D {
    double m00 = 1.0, m01 = 0.0, m02 = 0.0;
    double m10 = 0.0, m11 = 1.0, m12 = 0.0;

    [[nodiscard]] constexpr double applyX(double x, double y) const noexcept {
        return m00 * x + m01 * y + m02;
    }
    [[nodiscard]] constexpr double applyY(double x, double y) const noexcept {
        return m10 * x + m11 * y + m12;
    }
    [[nodiscard]] constexpr double determinant() const noexcept { return m00 * m11 - m01 * m10; }

    // Inverse transform. Only meaningful when determinant() is non-zero (callers check); a singular
    // matrix yields a zeroed linear part.
    [[nodiscard]] constexpr Affine2D inverted() const noexcept {
        const double det = determinant();
        const double inv = det != 0.0 ? 1.0 / det : 0.0;
        Affine2D r;
        r.m00 = m11 * inv;
        r.m01 = -m01 * inv;
        r.m10 = -m10 * inv;
        r.m11 = m00 * inv;
        r.m02 = -(r.m00 * m02 + r.m01 * m12);  // -R^-1 * t
        r.m12 = -(r.m10 * m02 + r.m11 * m12);
        return r;
    }

    static constexpr Affine2D translation(double tx, double ty) noexcept {
        return Affine2D{1.0, 0.0, tx, 0.0, 1.0, ty};
    }
    static constexpr Affine2D scaling(double sx, double sy) noexcept {
        return Affine2D{sx, 0.0, 0.0, 0.0, sy, 0.0};
    }
    static Affine2D rotation(double radians) noexcept {
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        return Affine2D{c, -s, 0.0, s, c, 0.0};
    }

    // a after b: result(p) == a(b(p)).
    [[nodiscard]] static constexpr Affine2D concat(const Affine2D& a, const Affine2D& b) noexcept {
        return Affine2D{
            a.m00 * b.m00 + a.m01 * b.m10,         a.m00 * b.m01 + a.m01 * b.m11,
            a.m00 * b.m02 + a.m01 * b.m12 + a.m02, a.m10 * b.m00 + a.m11 * b.m10,
            a.m10 * b.m01 + a.m11 * b.m11,         a.m10 * b.m02 + a.m11 * b.m12 + a.m12};
    }
};

}  // namespace pe
