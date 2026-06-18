#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <vector>

using namespace pe;

namespace {
int alphaAt(const Document& doc, LayerId id, int x, int y) {
    const auto* pl = static_cast<const PixelLayer*>(doc.findLayer(id));
    return pl->tiles().pixel(x, y).a;
}
}  // namespace

PE_TEST(selection_inactive_is_all_editable) {
    Selection s;
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
    PE_CHECK_NEAR(s.coverage(-100, 9999), 1.0f);
    PE_CHECK_EQ(s.value(5, 5), static_cast<uint8_t>(255));
}

PE_TEST(selection_rect) {
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    PE_CHECK(s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);    // inside
    PE_CHECK_NEAR(s.coverage(20, 20), 0.0f);  // outside
    PE_CHECK_EQ(s.value(5, 5), static_cast<uint8_t>(255));
    PE_CHECK_EQ(s.value(20, 20), static_cast<uint8_t>(0));

    s.selectNone();
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(20, 20), 1.0f);  // editable again
}

PE_TEST(selection_boolean_ops) {
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    s.addRect(Rect{20, 20, 5, 5});
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
    PE_CHECK_NEAR(s.coverage(22, 22), 1.0f);
    PE_CHECK_NEAR(s.coverage(15, 15), 0.0f);

    Selection sub;
    sub.selectRect(Rect{0, 0, 20, 20});
    sub.subtractRect(Rect{0, 0, 10, 20});
    PE_CHECK_NEAR(sub.coverage(5, 5), 0.0f);   // removed
    PE_CHECK_NEAR(sub.coverage(15, 5), 1.0f);  // kept

    Selection inter;
    inter.selectRect(Rect{0, 0, 20, 20});
    inter.intersectRect(Rect{10, 0, 20, 20});
    PE_CHECK_NEAR(inter.coverage(5, 5), 0.0f);   // outside intersection
    PE_CHECK_NEAR(inter.coverage(15, 5), 1.0f);  // inside intersection
}

PE_TEST(selection_invert) {
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    s.invert(Rect{0, 0, 20, 20});
    PE_CHECK_NEAR(s.coverage(5, 5), 0.0f);    // was selected -> now not
    PE_CHECK_NEAR(s.coverage(15, 15), 1.0f);  // was not -> now selected
}

PE_TEST(selection_thin_huge_rect_rejected) {
    // A thin but enormous rect spans too many tiles; it must be rejected (no
    // ~16 GB allocation), leaving the selection inactive (all editable).
    Selection s;
    s.selectRect(Rect{0, 0, 64'000'000, 1});
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(0));
}

PE_TEST(selection_addrect_empty_does_not_lock) {
    // addRect of an empty/invalid rect must not flip an inactive selection to
    // "active but empty" (which would block all editing).
    Selection s;
    s.addRect(Rect{});  // empty
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);  // still fully editable

    s.selectRect(Rect{0, 0, 10, 10});
    s.addRect(Rect{});  // no-op on an active selection
    PE_CHECK(s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
}

PE_TEST(selection_intersect_drops_dead_tiles) {
    // Intersecting non-overlapping regions yields an empty selection with no
    // dead tiles, so selectedBounds() is tight/empty.
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    s.intersectRect(Rect{1000, 1000, 10, 10});  // no overlap
    PE_CHECK_NEAR(s.coverage(5, 5), 0.0f);
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(0));
    PE_CHECK(s.selectedBounds().isEmpty());
}

PE_TEST(paint_is_gated_by_selection) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 64});  // left half only

    BrushSettings b;
    b.diameter = 8;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    std::vector<StrokePoint> pts = {{{4, 32}, 1.0f}, {{60, 32}, 1.0f}};  // crosses the boundary

    auto cmd = paintStroke(*doc, base, b, Rgbaf{1, 0, 0, 1}, pts, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK_EQ(alphaAt(*doc, base, 10, 32), 255);  // inside selection -> painted
    PE_CHECK_EQ(alphaAt(*doc, base, 50, 32), 0);    // outside selection -> blocked
}

PE_TEST(selection_save_to_mask) {
    Selection sel;
    sel.selectRect(Rect{2, 2, 3, 3});  // [2,5) x [2,5)
    PixelBuffer mask = sel.toMask(Rect{0, 0, 8, 8});
    PE_CHECK_EQ(mask.width(), 8);
    PE_CHECK_EQ(mask.at(2, 2), (Rgba8{255, 255, 255, 255}));  // selected -> white, opaque
    PE_CHECK_EQ(mask.at(4, 4).r, 255);
    PE_CHECK_EQ(mask.at(0, 0).r, 0);  // outside -> black
    PE_CHECK_EQ(mask.at(5, 5).r, 0);  // exclusive bottom-right
}

PE_TEST(selection_load_mask_roundtrip) {
    Selection a;
    a.selectRect(Rect{1, 1, 4, 2});
    PixelBuffer mask = a.toMask(Rect{0, 0, 8, 8});

    Selection b;
    b.loadMask(mask, 0, 0);
    PE_CHECK(b.active());
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) PE_CHECK_EQ(b.value(x, y), a.value(x, y));
    }
}

PE_TEST(selection_load_partial_coverage) {
    // A feathered (grayscale) mask loads as partial selection coverage.
    PixelBuffer mask(2, 1);
    mask.set(0, 0, Rgba8{128, 128, 128, 255});
    mask.set(1, 0, Rgba8{0, 0, 0, 255});
    Selection s;
    s.loadMask(mask, 0, 0);
    PE_CHECK_EQ(s.value(0, 0), static_cast<uint8_t>(128));  // partial
    PE_CHECK_NEAR(s.coverage(0, 0), 128.0f / 255.0f);
    PE_CHECK_EQ(s.value(1, 0), static_cast<uint8_t>(0));  // unselected
}

PE_TEST(selection_inactive_to_mask_is_full) {
    Selection s;  // inactive -> whole document selected
    PixelBuffer m = s.toMask(Rect{0, 0, 4, 4});
    PE_CHECK_EQ(m.at(0, 0).r, 255);
    PE_CHECK_EQ(m.at(3, 3).r, 255);
}

PE_TEST(selection_load_empty_deselects) {
    Selection s;
    s.selectRect(Rect{0, 0, 4, 4});
    s.loadMask(PixelBuffer{}, 0, 0);  // empty channel -> nothing selected
    PE_CHECK(!s.active());
}

PE_TEST(selection_loadmask_rejects_extreme_origin) {
    // A mask placed at an out-of-range origin must be rejected (no int overflow on
    // originX+x), leaving the selection inactive rather than corrupting tiles.
    Selection s;
    s.selectRect(Rect{0, 0, 4, 4});  // make it active first
    PixelBuffer mask(4, 4, Rgba8{255, 255, 255, 255});
    s.loadMask(mask, 1 << 28, 0);  // origin far beyond kCoordBound (~67M)
    PE_CHECK(!s.active());         // rejected -> nothing selected
}

PE_TEST(selection_tomask_rejects_out_of_range_bounds) {
    Selection s;
    s.selectRect(Rect{0, 0, 4, 4});
    PE_CHECK(s.toMask(Rect{1 << 28, 0, 4, 4}).isEmpty());  // out-of-range origin -> empty
}

PE_TEST(selection_tight_bounds_is_pixel_accurate) {
    // tightBounds returns the exact pixel extent, unlike the tile-granular selectedBounds.
    Selection s;
    PE_CHECK(s.tightBounds().isEmpty());  // inactive -> empty

    s.selectRect(Rect{8, 8, 16, 16});  // wholly inside tile (0,0)
    PE_CHECK_EQ(s.tightBounds(), (Rect{8, 8, 16, 16}));
    // selectedBounds snaps to the 256px tile, which is exactly why the ants need tightBounds.
    PE_CHECK_EQ(s.selectedBounds(), (Rect{0, 0, 256, 256}));

    // A selection straddling a tile boundary: the tight box still hugs the pixels, while the
    // tile-granular box spans both whole tiles.
    s.selectRect(Rect{250, 250, 20, 20});  // crosses into tiles (0,0),(1,0),(0,1),(1,1)
    PE_CHECK_EQ(s.tightBounds(), (Rect{250, 250, 20, 20}));
    PE_CHECK_EQ(s.selectedBounds(), (Rect{0, 0, 512, 512}));

    s.selectNone();
    PE_CHECK(s.tightBounds().isEmpty());
}

PE_TEST(selection_polygon_fills_interior) {
    Selection s;
    const std::vector<Point> degenerate = {{0, 0}, {5, 5}};
    s.selectPolygon(degenerate);
    PE_CHECK(!s.active());  // fewer than 3 vertices selects nothing

    const std::vector<Point> square = {{2, 2}, {12, 2}, {12, 12}, {2, 12}};
    s.selectPolygon(square);
    PE_CHECK(s.active());
    PE_CHECK_EQ(s.value(7, 7), static_cast<uint8_t>(255));  // interior filled
    PE_CHECK_EQ(s.value(0, 0), static_cast<uint8_t>(0));    // outside the polygon
    PE_CHECK_EQ(s.value(20, 20), static_cast<uint8_t>(0));  // far outside

    // Extreme vertex coordinates are rejected before the bbox extent is computed, so the
    // maxX-minX subtraction can never overflow int.
    const std::vector<Point> huge = {{-(1 << 27), 0}, {1 << 27, 0}, {0, 10}};
    s.selectPolygon(huge);
    PE_CHECK(!s.active());
}

PE_TEST(selection_magic_wand_contiguous_color) {
    PixelBuffer img(10, 10);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            img.set(x, y, x < 5 ? Rgba8{200, 30, 30, 255} : Rgba8{30, 30, 200, 255});
        }
    }
    Selection s = magicWandSelection(img, 1, 1, 10);  // seed in the red region, tolerance 10
    PE_CHECK(s.active());
    PE_CHECK_EQ(s.value(1, 1), static_cast<uint8_t>(255));  // seed selected
    PE_CHECK_EQ(s.value(2, 8), static_cast<uint8_t>(255));  // same contiguous red region
    PE_CHECK_EQ(s.value(8, 8), static_cast<uint8_t>(0));    // blue region: outside tolerance

    PE_CHECK(!magicWandSelection(img, -1, 0, 10).active());  // out-of-bounds seed -> inactive
    PE_CHECK(!magicWandSelection(img, 0, 99, 10).active());
}

PE_TEST(selection_grow_expands_boundary) {
    Selection s;
    s.selectRect(Rect{10, 10, 4, 4});  // x in [10,13], y in [10,13]
    s.grow(2);
    PE_CHECK(s.active());
    PE_CHECK_EQ(static_cast<int>(s.value(8, 11)), 255);   // 2px left of the edge -> now selected
    PE_CHECK_EQ(static_cast<int>(s.value(7, 11)), 0);     // 3px out -> still unselected
    PE_CHECK_EQ(static_cast<int>(s.value(12, 12)), 255);  // original interior stays selected
}

PE_TEST(selection_shrink_contracts_boundary) {
    Selection s;
    s.selectRect(Rect{8, 8, 16, 16});  // x,y in [8,23]
    s.shrink(2);
    PE_CHECK(s.active());
    PE_CHECK_EQ(static_cast<int>(s.value(8, 15)), 0);     // original edge -> eroded away
    PE_CHECK_EQ(static_cast<int>(s.value(11, 15)), 255);  // 3px in -> kept
}

PE_TEST(selection_shrink_to_nothing_deselects) {
    Selection s;
    s.selectRect(Rect{10, 10, 2, 2});
    s.shrink(5);  // erodes the whole 2x2 away
    PE_CHECK(!s.active());
}

// Regression for the audit H1: shrink must contract inward from a canvas-coincident edge, so
// Select All then shrink yields an inset (it previously was a complete no-op there).
PE_TEST(selection_shrink_insets_a_canvas_filling_selection) {
    Selection s;
    s.selectAll(Rect{0, 0, 32, 32});
    s.shrink(4);
    PE_CHECK(s.active());
    PE_CHECK_EQ(static_cast<int>(s.value(0, 16)), 0);     // canvas-edge column eroded
    PE_CHECK_EQ(static_cast<int>(s.value(2, 16)), 0);     // within 4px of the edge eroded
    PE_CHECK_EQ(static_cast<int>(s.value(16, 16)), 255);  // interior kept
}

// Regression for the audit H2: a refinement must not silently delete off-canvas coverage. A
// selection extending past the canvas, grown, keeps its off-canvas part.
PE_TEST(selection_grow_preserves_off_canvas_coverage) {
    Selection s;
    s.selectRect(Rect{-10, -10, 40, 40});                 // straddles the (implicit) canvas origin
    PE_CHECK_EQ(static_cast<int>(s.value(-5, -5)), 255);  // off-origin coverage exists
    s.grow(2);
    PE_CHECK(s.active());
    PE_CHECK_EQ(static_cast<int>(s.value(-5, -5)), 255);   // still selected after grow (not wiped)
    PE_CHECK_EQ(static_cast<int>(s.value(-12, -5)), 255);  // grew further out by 2px
}

PE_TEST(selection_feather_softens_edge) {
    const Rect canvas{0, 0, 48, 48};
    Selection s;
    s.selectRect(Rect{12, 12, 20, 20});  // hard-edged 20x20
    s.feather(2.0f, canvas);
    PE_CHECK(s.active());
    const int interior = static_cast<int>(s.value(22, 22));  // deep inside
    const int edge = static_cast<int>(s.value(12, 22));      // on the original boundary
    const int outside = static_cast<int>(s.value(9, 22));    // 3px outside
    PE_CHECK(interior > 180);                                // interior stays mostly selected
    PE_CHECK(edge > 40 && edge < 220);                       // boundary became partial coverage
    PE_CHECK(outside > 0);  // coverage bled outward (no longer a hard 0)
}

// Regression for the audit feather-MEDIUM: a canvas-filling selection must NOT fade at the canvas
// border (clamp-extend treats the canvas edge as "the selection continues").
PE_TEST(selection_feather_does_not_fade_canvas_border) {
    const Rect canvas{0, 0, 32, 32};
    Selection s;
    s.selectAll(canvas);
    s.feather(3.0f, canvas);
    PE_CHECK(s.active());
    PE_CHECK_EQ(static_cast<int>(s.value(0, 16)), 255);   // canvas-edge pixel stays fully selected
    PE_CHECK_EQ(static_cast<int>(s.value(16, 16)), 255);  // interior unchanged
}

PE_TEST(selection_refine_noops_when_inactive) {
    const Rect canvas{0, 0, 32, 32};
    Selection s;  // inactive
    s.grow(3);
    PE_CHECK(!s.active());
    s.shrink(3);
    PE_CHECK(!s.active());
    s.feather(3.0f, canvas);
    PE_CHECK(!s.active());
    s.grow(0);  // non-positive radius is a no-op too
    PE_CHECK(!s.active());
}

// A tiny-but-positive feather sigma must not produce a NaN-poisoned kernel (sigma is floored).
PE_TEST(selection_feather_tiny_sigma_is_safe) {
    const Rect canvas{0, 0, 32, 32};
    Selection s;
    s.selectRect(Rect{8, 8, 16, 16});
    s.feather(1e-30f, canvas);
    PE_CHECK(s.active());                                 // not NaN-wiped into deselection
    PE_CHECK_EQ(static_cast<int>(s.value(15, 15)), 255);  // interior intact
}
