#include "pe/core/Geometry.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(rect_empty_and_contains) {
    PE_CHECK(Rect{}.isEmpty());
    PE_CHECK(Rect{0, 0, -1, 5}.isEmpty());

    Rect r{10, 20, 30, 40};
    PE_CHECK(!r.isEmpty());
    PE_CHECK(r.contains(Point{10, 20}));   // top-left inclusive
    PE_CHECK(r.contains(Point{39, 59}));   // bottom-right inclusive edge
    PE_CHECK(!r.contains(Point{40, 60}));  // right/bottom exclusive
    PE_CHECK_EQ(r.right(), 40);
    PE_CHECK_EQ(r.bottom(), 60);
}

PE_TEST(rect_intersection) {
    Rect a{0, 0, 100, 100};
    Rect b{50, 50, 100, 100};
    PE_CHECK_EQ(a.intersected(b), (Rect{50, 50, 50, 50}));
    PE_CHECK(a.intersects(b));

    Rect c{200, 200, 10, 10};
    PE_CHECK(a.intersected(c).isEmpty());
    PE_CHECK(!a.intersects(c));
}

PE_TEST(rect_union) {
    Rect a{0, 0, 10, 10};
    Rect b{20, 20, 10, 10};
    PE_CHECK_EQ(a.united(b), (Rect{0, 0, 30, 30}));

    // Union with empty is identity.
    PE_CHECK_EQ(a.united(Rect{}), a);
    PE_CHECK_EQ(Rect{}.united(a), a);
}

PE_TEST(size_area) {
    PE_CHECK_EQ(Size(4, 5).area(), static_cast<int64_t>(20));
    PE_CHECK(Size(0, 5).isEmpty());
}

PE_TEST(affine_apply_and_factories) {
    const Affine2D t = Affine2D::translation(3.0, -2.0);
    PE_CHECK_NEAR(t.applyX(10.0, 5.0), 13.0);
    PE_CHECK_NEAR(t.applyY(10.0, 5.0), 3.0);

    const Affine2D s = Affine2D::scaling(2.0, 0.5);
    PE_CHECK_NEAR(s.applyX(4.0, 8.0), 8.0);
    PE_CHECK_NEAR(s.applyY(4.0, 8.0), 4.0);

    const Affine2D r = Affine2D::rotation(1.5707963267948966);  // +90deg: (1,0) -> (0,1)
    PE_CHECK_NEAR(r.applyX(1.0, 0.0), 0.0);
    PE_CHECK_NEAR(r.applyY(1.0, 0.0), 1.0);
}

PE_TEST(affine_inverse_and_concat) {
    // A non-trivial composed transform inverts back to the original point.
    const Affine2D a =
        Affine2D::concat(Affine2D::translation(5.0, 7.0),
                         Affine2D::concat(Affine2D::rotation(0.6), Affine2D::scaling(2.0, 3.0)));
    PE_CHECK(a.determinant() != 0.0);
    const Affine2D inv = a.inverted();
    const double px = 12.0;
    const double py = -4.0;
    const double tx = a.applyX(px, py);
    const double ty = a.applyY(px, py);
    PE_CHECK_NEAR(inv.applyX(tx, ty), px);
    PE_CHECK_NEAR(inv.applyY(tx, ty), py);

    // concat(a, b) applies b first, then a.
    const Affine2D ab =
        Affine2D::concat(Affine2D::translation(1.0, 0.0), Affine2D::scaling(2.0, 2.0));
    PE_CHECK_NEAR(ab.applyX(3.0, 0.0), 7.0);  // scale 3->6, then translate +1
}
