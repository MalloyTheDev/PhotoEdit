#include "pe/core/ViewTransform.hpp"
#include "pe_test.hpp"

#include <cmath>

using namespace pe;

namespace {
bool nd(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}
}  // namespace

PE_TEST(affine_compose_and_inverse) {
    Affine2 t = Affine2::translation(10, 20);
    Affine2 s = Affine2::scaling(2.0);
    Affine2 m = t * s;  // scale then translate
    PointD p = m.apply(PointD{3, 4});
    PE_CHECK(nd(p.x, 16));  // 3*2 + 10
    PE_CHECK(nd(p.y, 28));  // 4*2 + 20

    PointD back = m.inverse().apply(p);
    PE_CHECK(nd(back.x, 3));
    PE_CHECK(nd(back.y, 4));
}

PE_TEST(affine_rotation_quarter_turn) {
    Affine2 r = Affine2::rotation(M_PI / 2);  // 90 degrees
    PointD p = r.apply(PointD{1, 0});
    PE_CHECK(nd(p.x, 0, 1e-9));
    PE_CHECK(nd(p.y, 1, 1e-9));
}

PE_TEST(view_identity) {
    ViewTransform v;
    PointD p = v.docToView(PointD{12, 34});
    PE_CHECK(nd(p.x, 12));
    PE_CHECK(nd(p.y, 34));
    PointD q = v.viewToDoc(PointD{12, 34});
    PE_CHECK(nd(q.x, 12));
    PE_CHECK(nd(q.y, 34));
}

PE_TEST(view_zoom_about_origin) {
    ViewTransform v;
    v.setZoom(2.0);  // focus at origin by default
    PointD p = v.docToView(PointD{10, 10});
    PE_CHECK(nd(p.x, 20));
    PE_CHECK(nd(p.y, 20));
}

PE_TEST(view_zoom_clamped) {
    ViewTransform v;
    v.setZoom(1000.0);
    PE_CHECK(nd(v.zoom(), kMaxZoom));
    v.setZoom(0.0);
    PE_CHECK(nd(v.zoom(), kMinZoom));
}

PE_TEST(view_roundtrip_with_rotation) {
    ViewTransform v;
    v.setZoom(1.5);
    v.setRotation(0.7);
    v.setFocus(PointD{50, 60}, PointD{200, 100});
    PointD doc{123.0, 45.0};
    PointD view = v.docToView(doc);
    PointD back = v.viewToDoc(view);
    PE_CHECK(nd(back.x, doc.x, 1e-6));
    PE_CHECK(nd(back.y, doc.y, 1e-6));
}

PE_TEST(view_zoom_around_keeps_point_fixed) {
    ViewTransform v;
    v.setZoom(1.0);
    v.setRotation(0.3);
    v.setFocus(PointD{0, 0}, PointD{0, 0});

    const PointD anchorView{300, 220};
    const PointD docUnder = v.viewToDoc(anchorView);
    v.zoomAround(anchorView, 4.0);
    // The document point under the cursor must be unchanged after zoom.
    const PointD nowView = v.docToView(docUnder);
    PE_CHECK(nd(nowView.x, anchorView.x, 1e-6));
    PE_CHECK(nd(nowView.y, anchorView.y, 1e-6));
    PE_CHECK(nd(v.zoom(), 4.0));
}

PE_TEST(view_visible_doc_rect) {
    ViewTransform v;  // identity
    PE_CHECK_EQ(v.visibleDocRect(Size{800, 600}), (Rect{0, 0, 800, 600}));

    v.setZoom(2.0);  // 2x => half the document is visible
    PE_CHECK_EQ(v.visibleDocRect(Size{800, 600}), (Rect{0, 0, 400, 300}));
}

PE_TEST(view_setters_reject_nonfinite) {
    ViewTransform v;
    v.setZoom(2.0);
    v.setZoom(std::nan(""));  // ignored
    PE_CHECK(nd(v.zoom(), 2.0));
    v.setRotation(0.5);
    v.setRotation(INFINITY);  // ignored
    PE_CHECK(nd(v.rotation(), 0.5));
}

PE_TEST(view_visible_rect_extreme_no_overflow) {
    // Extreme zoom-out + far pan must not overflow the int casts (UBSan guards
    // the actual UB; here we just assert a valid, non-degenerate rect comes back).
    ViewTransform v;
    v.setZoom(kMinZoom);
    v.setFocus(PointD{250000, 250000}, PointD{0, 0});
    Rect r = v.visibleDocRect(Size{8000, 8000});
    PE_CHECK(!r.isEmpty());
}

PE_TEST(view_pan) {
    ViewTransform v;
    v.panByView(100, 0);  // move pinned doc-origin 100px right on screen
    PointD p = v.docToView(PointD{0, 0});
    PE_CHECK(nd(p.x, 100));
    PE_CHECK(nd(p.y, 0));
}
