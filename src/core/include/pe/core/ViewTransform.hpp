#pragma once

#include "pe/core/Geometry.hpp"

namespace pe {

// A floating-point 2D point in document or view space. The integer `Point` is for
// pixel-addressable storage; view math needs sub-pixel precision.
struct PointD {
    double x = 0.0;
    double y = 0.0;
};

// A 2D affine transform mapping (x,y) -> (a*x + c*y + e, b*x + d*y + f). Used for
// the view transform now and reused by the transform system later. Pure value
// type, no Qt.
struct Affine2 {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

    [[nodiscard]] static Affine2 identity() noexcept { return Affine2{}; }
    [[nodiscard]] static Affine2 translation(double tx, double ty) noexcept {
        return Affine2{1.0, 0.0, 0.0, 1.0, tx, ty};
    }
    [[nodiscard]] static Affine2 scaling(double s) noexcept {
        return Affine2{s, 0.0, 0.0, s, 0.0, 0.0};
    }
    [[nodiscard]] static Affine2 rotation(double rad) noexcept;

    // Compose: (this * other) maps p -> this(other(p)).
    [[nodiscard]] Affine2 operator*(const Affine2& o) const noexcept;
    [[nodiscard]] Affine2 inverse() const noexcept;
    [[nodiscard]] PointD apply(PointD p) const noexcept {
        return PointD{a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }
};

inline constexpr double kMinZoom = 0.01;  // 1%
inline constexpr double kMaxZoom = 64.0;  // 6400%

// Maps document pixel space <-> view (device-pixel) space. Zoom and view rotation
// are properties of presentation; the document is never resampled. A focus pair
// pins a document point under a view point so zoom/rotate happen about it. See
// docs/systems/02-canvas-rendering.md.
class ViewTransform {
public:
    [[nodiscard]] double zoom() const noexcept { return zoom_; }
    [[nodiscard]] double rotation() const noexcept { return rotation_; }
    [[nodiscard]] PointD focusDoc() const noexcept { return focusDoc_; }
    [[nodiscard]] PointD focusView() const noexcept { return focusView_; }

    // All setters reject non-finite (NaN/Inf) input and leave the field unchanged,
    // since these are reachable from UI/tablet input and would otherwise poison the
    // transform and the int casts in visibleDocRect.
    void setZoom(double z) noexcept;
    void setRotation(double rad) noexcept;
    void setFocus(PointD docPx, PointD viewPx) noexcept;

    [[nodiscard]] Affine2 docToView() const noexcept;
    [[nodiscard]] Affine2 viewToDoc() const noexcept { return docToView().inverse(); }

    [[nodiscard]] PointD docToView(PointD docPx) const noexcept { return docToView().apply(docPx); }
    [[nodiscard]] PointD viewToDoc(PointD viewPx) const noexcept {
        return viewToDoc().apply(viewPx);
    }

    // Integer document-space bounding rect of a viewport of size `vp` (device px).
    [[nodiscard]] Rect visibleDocRect(Size vp) const noexcept;

    // Zoom while keeping the document point currently under `viewPx` stationary.
    void zoomAround(PointD viewPx, double newZoom) noexcept;
    // Pan by a view-space delta (moves the pinned doc point on screen).
    void panByView(double dxView, double dyView) noexcept;

private:
    double zoom_ = 1.0;
    double rotation_ = 0.0;
    PointD focusDoc_{0.0, 0.0};
    PointD focusView_{0.0, 0.0};
};

}  // namespace pe
