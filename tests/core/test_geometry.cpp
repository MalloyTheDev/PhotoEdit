#include "pe/core/Geometry.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(rect_empty_and_contains) {
    PE_CHECK(Rect{}.isEmpty());
    PE_CHECK(Rect{0, 0, -1, 5}.isEmpty());

    Rect r{10, 20, 30, 40};
    PE_CHECK(!r.isEmpty());
    PE_CHECK(r.contains(Point{10, 20}));     // top-left inclusive
    PE_CHECK(r.contains(Point{39, 59}));     // bottom-right inclusive edge
    PE_CHECK(!r.contains(Point{40, 60}));    // right/bottom exclusive
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
