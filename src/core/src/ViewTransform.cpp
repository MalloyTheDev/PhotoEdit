#include "pe/core/ViewTransform.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace pe {

Affine2 Affine2::rotation(double rad) noexcept {
    const double cs = std::cos(rad);
    const double sn = std::sin(rad);
    return Affine2{cs, sn, -sn, cs, 0.0, 0.0};
}

Affine2 Affine2::operator*(const Affine2& o) const noexcept {
    return Affine2{
        a * o.a + c * o.b,      // a
        b * o.a + d * o.b,      // b
        a * o.c + c * o.d,      // c
        b * o.c + d * o.d,      // d
        a * o.e + c * o.f + e,  // e
        b * o.e + d * o.f + f,  // f
    };
}

Affine2 Affine2::inverse() const noexcept {
    const double det = a * d - c * b;
    // Use an epsilon, not == 0: a near-singular matrix would otherwise yield
    // huge/Inf entries that become UB when cast to int downstream. Reusable type,
    // so guard defensively even though the view transform is never singular.
    if (std::abs(det) < 1e-12) return Affine2::identity();
    const double inv = 1.0 / det;
    const double ia = d * inv;
    const double ib = -b * inv;
    const double ic = -c * inv;
    const double id = a * inv;
    return Affine2{ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
}

void ViewTransform::setZoom(double z) noexcept {
    if (!std::isfinite(z)) return;
    zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
}

void ViewTransform::setRotation(double rad) noexcept {
    if (!std::isfinite(rad)) return;
    rotation_ = rad;
}

void ViewTransform::setFocus(PointD docPx, PointD viewPx) noexcept {
    if (!std::isfinite(docPx.x) || !std::isfinite(docPx.y) || !std::isfinite(viewPx.x) ||
        !std::isfinite(viewPx.y)) {
        return;
    }
    focusDoc_ = docPx;
    focusView_ = viewPx;
}

Affine2 ViewTransform::docToView() const noexcept {
    // T(focusView) * R(rotation) * S(zoom) * T(-focusDoc)
    return Affine2::translation(focusView_.x, focusView_.y) * Affine2::rotation(rotation_) *
           Affine2::scaling(zoom_) * Affine2::translation(-focusDoc_.x, -focusDoc_.y);
}

Rect ViewTransform::visibleDocRect(Size vp) const noexcept {
    if (vp.isEmpty()) return Rect{};
    const Affine2 v2d = viewToDoc();
    const std::array<PointD, 4> corners = {
        v2d.apply(PointD{0.0, 0.0}),
        v2d.apply(PointD{static_cast<double>(vp.width), 0.0}),
        v2d.apply(PointD{0.0, static_cast<double>(vp.height)}),
        v2d.apply(PointD{static_cast<double>(vp.width), static_cast<double>(vp.height)}),
    };
    double minX = corners[0].x, maxX = corners[0].x;
    double minY = corners[0].y, maxY = corners[0].y;
    for (const PointD& p : corners) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    // Clamp before the int cast: extreme zoom-out + far pan can push corners far
    // beyond INT range, where static_cast<int> is UB. A generous finite bound
    // keeps the result a valid Rect (its width = x1-x0 stays within int).
    const auto toInt = [](double v) -> int {
        constexpr double lo = -(1 << 29);  // ~ -5.4e8, well inside int
        constexpr double hi = (1 << 29);
        if (std::isnan(v)) return 0;
        return static_cast<int>(std::clamp(v, lo, hi));
    };
    const int x0 = toInt(std::floor(minX));
    const int y0 = toInt(std::floor(minY));
    const int x1 = toInt(std::ceil(maxX));
    const int y1 = toInt(std::ceil(maxY));
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

void ViewTransform::zoomAround(PointD viewPx, double newZoom) noexcept {
    // Pin the document point currently under viewPx, then change only the zoom.
    focusDoc_ = viewToDoc(viewPx);
    focusView_ = viewPx;
    setZoom(newZoom);
}

void ViewTransform::panByView(double dxView, double dyView) noexcept {
    focusView_.x += dxView;
    focusView_.y += dyView;
}

}  // namespace pe
