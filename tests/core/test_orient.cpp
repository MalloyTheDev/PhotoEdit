// The exact orientation geometry behind Image Rotation. These maps are the foundation the pixel,
// mask, selection and canvas remaps all build on, so the round-trip (forward then inverse is the
// identity) and the canvas-size rule are pinned here before anything depends on them.

#include "pe/core/Orient.hpp"
#include "pe_test.hpp"

using namespace pe;

namespace {
constexpr Orient kAll[] = {Orient::FlipHorizontal, Orient::FlipVertical, Orient::Rotate180,
                           Orient::Rotate90CW, Orient::Rotate90CCW};
}

PE_TEST(orient_canvas_size_swaps_only_on_quarter_turns) {
    const Size s{40, 30};
    PE_CHECK(orientedCanvas(Orient::FlipHorizontal, s) == (Size{40, 30}));
    PE_CHECK(orientedCanvas(Orient::FlipVertical, s) == (Size{40, 30}));
    PE_CHECK(orientedCanvas(Orient::Rotate180, s) == (Size{40, 30}));
    PE_CHECK(orientedCanvas(Orient::Rotate90CW, s) == (Size{30, 40}));
    PE_CHECK(orientedCanvas(Orient::Rotate90CCW, s) == (Size{30, 40}));
}

PE_TEST(orient_forward_maps_known_corners) {
    const Size s{4, 3};  // W=4, H=3
    PE_CHECK(orientForward(Orient::FlipHorizontal, s, Point{0, 0}) == (Point{3, 0}));
    PE_CHECK(orientForward(Orient::FlipVertical, s, Point{0, 0}) == (Point{0, 2}));
    PE_CHECK(orientForward(Orient::Rotate180, s, Point{0, 0}) == (Point{3, 2}));
    // 90 CW: (x,y) -> (H-1-y, x). Top-left goes to the top-right of the H x W canvas.
    PE_CHECK(orientForward(Orient::Rotate90CW, s, Point{0, 0}) == (Point{2, 0}));
    PE_CHECK(orientForward(Orient::Rotate90CW, s, Point{0, 2}) == (Point{0, 0}));
    // 90 CCW: (x,y) -> (y, W-1-x).
    PE_CHECK(orientForward(Orient::Rotate90CCW, s, Point{0, 0}) == (Point{0, 3}));
    PE_CHECK(orientForward(Orient::Rotate90CCW, s, Point{3, 0}) == (Point{0, 0}));
}

PE_TEST(orient_inverse_undoes_forward_over_the_whole_canvas) {
    const Size s{7, 5};
    for (Orient op : kAll) {
        for (int y = 0; y < s.height; ++y) {
            for (int x = 0; x < s.width; ++x) {
                const Point fwd = orientForward(op, s, Point{x, y});
                // Inverse uses the SOURCE canvas dims (s); it must return the original point.
                PE_CHECK(orientInverse(op, s, fwd) == (Point{x, y}));
            }
        }
    }
}

PE_TEST(orient_forward_stays_in_the_new_canvas_bounds) {
    // Every on-canvas source lands on-canvas in the reoriented frame (no pixel falls off).
    const Size s{7, 5};
    for (Orient op : kAll) {
        const Size out = orientedCanvas(op, s);
        for (int y = 0; y < s.height; ++y) {
            for (int x = 0; x < s.width; ++x) {
                const Point p = orientForward(op, s, Point{x, y});
                PE_CHECK(p.x >= 0 && p.x < out.width && p.y >= 0 && p.y < out.height);
            }
        }
    }
}

PE_TEST(orient_rect_maps_a_row_to_a_column_on_a_quarter_turn) {
    const Size s{4, 3};
    // The whole top row {0,0,4,1} rotated 90 CW becomes a 1-wide, 4-tall column.
    const Rect r = orientRect(Orient::Rotate90CW, s, Rect{0, 0, 4, 1});
    PE_CHECK(r == (Rect{2, 0, 1, 4}));
    // A flip keeps the shape, mirrored.
    PE_CHECK(orientRect(Orient::FlipHorizontal, s, Rect{0, 0, 1, 3}) == (Rect{3, 0, 1, 3}));
    PE_CHECK(orientRect(Orient::FlipHorizontal, s, Rect{}).isEmpty());
}
