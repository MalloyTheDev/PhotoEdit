#pragma once

#include <algorithm>
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
    [[nodiscard]] constexpr bool isEmpty() const noexcept {
        return width <= 0 || height <= 0;
    }
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

    [[nodiscard]] constexpr bool isEmpty() const noexcept {
        return width <= 0 || height <= 0;
    }

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

} // namespace pe
