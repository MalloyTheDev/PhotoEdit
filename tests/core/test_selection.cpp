#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <utility>
#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
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

PE_TEST(magic_wand_rejects_extreme_aspect_image) {
    // 1 x 1048833 passes the pixel cap (~1M < 64M) but spans >4096 tile rows, so loadMask would
    // discard any result. magicWand now funnels through rejectFill and bails up front — no crash,
    // no full O(pixels) flood, and an inactive (empty) selection.
    PixelBuffer img(1, 1'048'833, Rgba8{255, 0, 0, 255});
    const Selection sel = magicWandSelection(img, 0, 0, 10);
    PE_CHECK(!sel.active());
    // A normal small image still floods correctly (the funnel didn't over-reject).
    PixelBuffer ok(8, 8, Rgba8{255, 0, 0, 255});
    PE_CHECK(magicWandSelection(ok, 0, 0, 10).active());
}

// ---------------------------------------------------------------------------
// Inverting the two representations of "everything selected".
//
// An inactive selection means everything is editable: coverage() is 1.0 and
// value() is 255 everywhere, and selectAll() says so explicitly when it falls back
// to inactive. So inverting an inactive selection must select nothing, exactly as
// inverting an explicit Select All does. invert() read stored() rather than the
// effective value, which treats inactive as empty and so inverted it to a FULL
// selection: inverting Select All left everything selected.
// ---------------------------------------------------------------------------

PE_TEST(selection_invert_of_inactive_selects_nothing) {
    Selection s;
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);  // inactive reads as everything selected

    s.invert(Rect{0, 0, 20, 20});

    PE_CHECK(s.active());  // now a real selection, not the implicit everything
    PE_CHECK_NEAR(s.coverage(0, 0), 0.0f);
    PE_CHECK_NEAR(s.coverage(5, 5), 0.0f);
    PE_CHECK_NEAR(s.coverage(19, 19), 0.0f);
}

PE_TEST(selection_invert_agrees_across_both_forms_of_select_all) {
    // Inactive and an explicit Select All mean the same thing, so inverting them
    // must give the same result. This is the invariant the bug broke.
    const Rect canvas{0, 0, 20, 20};

    Selection implicitAll;  // inactive
    implicitAll.invert(canvas);

    Selection explicitAll;
    explicitAll.selectAll(canvas);
    explicitAll.invert(canvas);

    for (const int p : {0, 5, 19}) {
        PE_CHECK_NEAR(implicitAll.coverage(p, p), explicitAll.coverage(p, p));
        PE_CHECK_NEAR(implicitAll.coverage(p, p), 0.0f);
    }
}

PE_TEST(selection_invert_twice_from_inactive_reselects_everything) {
    Selection s;
    const Rect canvas{0, 0, 20, 20};
    s.invert(canvas);  // everything -> nothing
    s.invert(canvas);  // nothing -> everything
    PE_CHECK_NEAR(s.coverage(0, 0), 1.0f);
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
    PE_CHECK_NEAR(s.coverage(19, 19), 1.0f);
}

PE_TEST(selection_feather_preserves_off_canvas_coverage) {
    // feather clamps its working region to the canvas on purpose, so a canvas-filling
    // selection is not faded at the border. But it then handed that clamped region to
    // loadMask, which clears every tile first, so any coverage outside the canvas was
    // destroyed rather than merely left alone. grow and shrink deliberately preserve it
    // (Selection.hpp says so), and a selection can hold off-canvas coverage: fillRect
    // does not clamp, and CropCommand shifts selections across the origin.
    const Rect canvas{0, 0, 64, 64};
    Selection s;
    s.selectRect(Rect{-20, -20, 30, 30});  // straddles the canvas origin
    PE_CHECK_EQ(static_cast<int>(s.value(-10, -10)), 255);
    PE_CHECK_EQ(static_cast<int>(s.value(5, 5)), 255);

    s.feather(2.0f, canvas);
    PE_CHECK(s.active());

    // Well outside the canvas and well inside the original rect: untouched by a blur
    // whose region stops at the canvas edge, so it must still be fully selected.
    PE_CHECK_EQ(static_cast<int>(s.value(-18, -18)), 255);
    PE_CHECK_EQ(static_cast<int>(s.value(-15, -15)), 255);
    // The on-canvas part still got feathered.
    const int inside = static_cast<int>(s.value(3, 3));
    PE_CHECK(inside > 0);
}

PE_TEST(selection_grow_and_feather_agree_about_off_canvas) {
    // The three refinements should not disagree about whether the exterior exists.
    const Rect canvas{0, 0, 64, 64};
    Selection a;
    a.selectRect(Rect{-10, 20, 40, 20});
    a.grow(2);
    PE_CHECK(a.value(-11, 30) > 0);  // grow reaches further off-canvas

    Selection b;
    b.selectRect(Rect{-10, 20, 40, 20});
    b.feather(2.0f, canvas);
    PE_CHECK_EQ(static_cast<int>(b.value(-8, 30)), 255);  // feather leaves it alone
}

namespace {

// The per-pixel semantics the run-based writer replaces, written out independently so a bug
// in the new traversal cannot also be in the reference. Mirrors setValue exactly, including
// "never allocate a tile just to write a zero", plus dropEmptyTiles.
//
// Same role the per-pixel accessor played as an oracle for the .pedoc gather in #176: the
// abandoned path is the thing that says what the fast path must agree with.
struct RefSelection {
    std::map<std::pair<int, int>, std::array<std::uint8_t, kTilePixels>> tiles;

    static int local(int c) {
        int m = c % kTileSize;
        if (m < 0) m += kTileSize;
        return m;
    }
    void set(int x, int y, std::uint8_t v) {
        const std::pair<int, int> k{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
        auto it = tiles.find(k);
        if (it == tiles.end()) {
            if (v == 0) return;  // do not allocate a tile to store a zero
            it = tiles.emplace(k, std::array<std::uint8_t, kTilePixels>{}).first;
        }
        it->second[static_cast<std::size_t>(local(y)) * kTileSize +
                   static_cast<std::size_t>(local(x))] = v;
    }
    [[nodiscard]] std::uint8_t at(int x, int y) const {
        const auto it = tiles.find({floorDiv(x, kTileSize), floorDiv(y, kTileSize)});
        if (it == tiles.end()) return 0;
        return it->second[static_cast<std::size_t>(local(y)) * kTileSize +
                          static_cast<std::size_t>(local(x))];
    }
    void dropEmpty() {
        for (auto it = tiles.begin(); it != tiles.end();) {
            const bool allZero = std::all_of(it->second.begin(), it->second.end(),
                                             [](std::uint8_t v) { return v == 0; });
            it = allZero ? tiles.erase(it) : std::next(it);
        }
    }
};

// Compare coverage AND the tile set. tileCount is the canonical-form half: a spurious
// all-zero tile is invisible to value() and to tightBounds(), but the defaulted operator==
// compares the whole map, so two selections with identical coverage can still differ.
bool matchesOracle(const Selection& sel, const RefSelection& ref, Rect probe) {
    if (sel.tileCount() != ref.tiles.size()) return false;
    for (int y = probe.top(); y < probe.bottom(); ++y) {
        for (int x = probe.left(); x < probe.right(); ++x) {
            if (sel.value(x, y) != ref.at(x, y)) return false;
        }
    }
    return true;
}

Rect grown(Rect r, int by) {
    return Rect{r.x - by, r.y - by, r.width + 2 * by, r.height + 2 * by};
}

// The geometries that matter for a run-splitting writer: edges landing exactly on, just
// before and just after a tile boundary; spans crossing several tiles on each axis; and
// negative coordinates, where a truncating divide lands in the wrong tile.
std::vector<Rect> boundaryRects() {
    constexpr int T = kTileSize;
    return {
        Rect{T - 1, T - 1, 1, 1},
        Rect{T, T, 1, 1},
        Rect{T + 1, T + 1, 1, 1},
        Rect{T - 1, 0, 2, 1},
        Rect{T - 1, 0, 2, T + 2},
        Rect{0, 0, T, T},
        Rect{0, 0, T + 1, T + 1},
        Rect{5, 5, 1, 3 * T},
        Rect{5, 5, 3 * T, 1},
        Rect{-1, -1, 2, 2},
        Rect{-T, -T, 1, 1},
        Rect{-T - 1, -T - 1, 1, 1},
        Rect{-T - 1, -T - 1, T + 2, T + 2},
    };
}

}  // namespace

PE_TEST(selection_fill_matches_the_per_pixel_oracle_at_tile_boundaries) {
    for (const Rect r : boundaryRects()) {
        Selection s;
        s.selectRect(r);
        RefSelection ref;
        for (int y = r.top(); y < r.bottom(); ++y) {
            for (int x = r.left(); x < r.right(); ++x) ref.set(x, y, 255);
        }
        ref.dropEmpty();
        // Probe two pixels beyond every edge, so a run that emitted one pixel too many or
        // too few shows up rather than hiding inside the filled area.
        PE_CHECK(matchesOracle(s, ref, grown(r, 2)));
    }
}

PE_TEST(selection_load_mask_matches_the_oracle_at_tile_boundaries) {
    // The mask path is the harder one: the source stride and the tile stride disagree, and
    // the origin is not tile-aligned. Origins include a negative one, which is the shape a
    // crop produces.
    constexpr int T = kTileSize;
    for (const Point origin : {Point{0, 0}, Point{7, 3}, Point{T - 1, 1}, Point{-T - 7, -5}}) {
        for (const Rect r : boundaryRects()) {
            if (r.width > 3 * T || r.height > 3 * T) continue;  // keep the buffers small
            PixelBuffer mask(r.width, r.height);
            for (int y = 0; y < r.height; ++y) {
                for (int x = 0; x < r.width; ++x) {
                    // A value that varies, so a misplaced run is visible as a wrong VALUE and
                    // not merely as wrong coverage.
                    const auto v = static_cast<std::uint8_t>(((x * 7 + y * 13) % 255) + 1);
                    mask.set(x, y, Rgba8{v, v, v, 255});
                }
            }
            Selection s;
            s.loadMask(mask, origin.x, origin.y);
            RefSelection ref;
            for (int y = 0; y < r.height; ++y) {
                for (int x = 0; x < r.width; ++x) {
                    ref.set(origin.x + x, origin.y + y, mask.at(x, y).r);
                }
            }
            ref.dropEmpty();
            PE_CHECK(matchesOracle(s, ref, grown(Rect{origin.x, origin.y, r.width, r.height}, 2)));
        }
    }
}

PE_TEST(selection_invert_matches_the_oracle_across_tiles) {
    // invert reads and writes the same run, so one tile resolution now serves both. Checked
    // for an active selection straddling tiles and for an inactive one, whose polarity is
    // the case that once made inverting Select All leave everything selected.
    constexpr int T = kTileSize;
    const Rect canvas{-T - 3, -T - 3, 3 * T + 7, 2 * T + 5};

    Selection active;
    active.selectRect(Rect{-5, -5, T + 20, T + 9});
    Selection inactive;

    for (Selection* sel : {&active, &inactive}) {
        const bool wasActive = sel->active();
        const Rect probe = grown(canvas, 2 * kTileSize);
        RefSelection ref;
        // Seed the oracle with the state that already exists. invert() only rewrites the rect
        // it is given, and part of the selection deliberately lies outside it: that coverage
        // must survive untouched, which an oracle built from nothing would miss.
        for (int y = probe.top(); y < probe.bottom(); ++y) {
            for (int x = probe.left(); x < probe.right(); ++x) {
                if (wasActive) ref.set(x, y, sel->value(x, y));
            }
        }
        for (int y = canvas.top(); y < canvas.bottom(); ++y) {
            for (int x = canvas.left(); x < canvas.right(); ++x) {
                const auto cur = wasActive ? sel->value(x, y) : static_cast<std::uint8_t>(255);
                ref.set(x, y, static_cast<std::uint8_t>(255 - cur));
            }
        }
        ref.dropEmpty();
        sel->invert(canvas);
        PE_CHECK(matchesOracle(*sel, ref, probe));
    }
}

PE_TEST(selection_never_allocates_a_tile_for_a_zero_run) {
    // The canonical-form rule. tiles_ must hold no all-zero tile, because selectedBounds()
    // and the defaulted operator== both read the map directly. dropEmptyTiles() erases them
    // after the fact, so the OUTPUT was always right; what the rule prevents is materialising
    // the whole region first, which on a large canvas is hundreds of megabytes.
    constexpr int T = kTileSize;

    // A non-empty but entirely black mask selects nothing and allocates nothing.
    PixelBuffer black(3 * T, 3 * T);
    Selection s;
    s.loadMask(black, 0, 0);
    PE_CHECK(s.active());
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(0));
    PE_CHECK(s.selectedBounds().isEmpty());

    // Inverting an INACTIVE selection writes 255 - 255 == 0 for every pixel of the canvas.
    // And it must not allocate them and then drop them: dropEmptyTiles() would hide that in
    // the final count, while the peak on a canvas-sized write is what the rule is for.
    const std::uint64_t allocsBefore = maskTileAllocCount();
    Selection inv;
    inv.invert(Rect{0, 0, 3 * T, 3 * T});
    PE_CHECK_EQ(inv.tileCount(), static_cast<std::size_t>(0));
    PE_CHECK_EQ(maskTileAllocCount() - allocsBefore, static_cast<std::uint64_t>(0));

    // A sparse mask allocates only the tiles its content touches, not the tiles its bounding
    // box spans. Two small blobs with three whole tiles of gap between them.
    PixelBuffer sparse(6 * T, 2 * T);
    for (int y = 4; y < 12; ++y) {
        for (int x = 4; x < 12; ++x) sparse.set(x, y, Rgba8{255, 255, 255, 255});
        for (int x = 5 * T + 4; x < 5 * T + 12; ++x) sparse.set(x, y, Rgba8{255, 255, 255, 255});
    }
    const std::uint64_t sparseBefore = maskTileAllocCount();
    Selection two;
    two.loadMask(sparse, 0, 0);
    PE_CHECK_EQ(two.tileCount(), static_cast<std::size_t>(2));  // not the 12 the bbox spans
    // Two allocated, not twelve allocated and ten erased.
    PE_CHECK_EQ(maskTileAllocCount() - sparseBefore, static_cast<std::uint64_t>(2));
    PE_CHECK_EQ(static_cast<int>(two.value(3 * T, 8)), 0);  // and the gap really is empty
}

PE_TEST(selection_write_resolves_one_tile_per_run_not_per_pixel) {
    // The defect this replaces: every mask write did one std::map::find PER PIXEL, over a
    // canvas-sized rect. A magic wand click on a 24 MP document therefore performed 24
    // million tree walks, over a map whose nodes each hold a 64 KiB tile inline. The lookup
    // count must scale with rows times tile COLUMNS, never with the pixel count.
    constexpr int T = kTileSize;
    const auto columnsSpanned = [](int x, int width) {
        if (width <= 0) return 0;
        return floorDiv(x + width - 1, kTileSize) - floorDiv(x, kTileSize) + 1;
    };

    for (const Rect r : {Rect{0, 0, 4 * T, 3 * T}, Rect{7, 9, 2 * T + 40, T + 5}, Rect{0, 0, 1, 1},
                         Rect{T - 1, 0, 2, 40}, Rect{-T - 3, -T - 3, 2 * T, 40}}) {
        const std::uint64_t before = maskWriteTileLookupCount();
        Selection s;
        s.selectRect(r);
        const std::uint64_t used = maskWriteTileLookupCount() - before;
        const auto want = static_cast<std::uint64_t>(r.height) *
                          static_cast<std::uint64_t>(columnsSpanned(r.x, r.width));
        PE_CHECK_EQ(used, want);
    }

    // And the shape that matters in practice: a full-canvas fill is rows times columns, not
    // rows times pixels. 4000 x 24 rather than 24,000,000.
    const std::uint64_t before = maskWriteTileLookupCount();
    Selection big;
    big.selectRect(Rect{0, 0, 6000, 4000});
    const std::uint64_t used = maskWriteTileLookupCount() - before;
    PE_CHECK_EQ(used, static_cast<std::uint64_t>(4000) * 24);  // 96,000
    // 24,000,000 / 96,000 == 250, the same ratio the .pedoc gather saw in #176 and for the
    // same reason: one lookup covers up to kTileSize samples. Guarded, because a writer that
    // stopped using setRun entirely would leave `used` at zero and divide by it, and a test
    // that crashes says far less than one that fails.
    PE_REQUIRE(used > 0);
    PE_CHECK_EQ(static_cast<std::uint64_t>(6000) * 4000 / used, static_cast<std::uint64_t>(250));
}

// --- the selection outline the marching ants are drawn from --------------------------------

namespace {

// The four segments that bound `r`, in the order and orientation outline() emits them, so a
// test can say "this outline is exactly this rectangle" rather than counting.
bool outlineIsRect(const std::vector<pe::OutlineSegment>& segs, pe::Rect r) {
    if (segs.size() != 4) return false;
    const pe::OutlineSegment want[4] = {
        {pe::Point{r.left(), r.top()}, pe::Point{r.right(), r.top()}},        // top
        {pe::Point{r.left(), r.top()}, pe::Point{r.left(), r.bottom()}},      // left
        {pe::Point{r.right(), r.top()}, pe::Point{r.right(), r.bottom()}},    // right
        {pe::Point{r.left(), r.bottom()}, pe::Point{r.right(), r.bottom()}},  // bottom
    };
    for (const pe::OutlineSegment& w : want) {
        if (std::find(segs.begin(), segs.end(), w) == segs.end()) return false;
    }
    return true;
}

}  // namespace

PE_TEST(selection_outline_of_a_rectangle_is_four_merged_edges) {
    // The runs have to merge, or a 4000 pixel wide selection emits 4000 unit segments per
    // edge and the overlay costs more to draw than the image beneath it.
    pe::Selection s;
    s.selectRect(pe::Rect{10, 20, 5, 7});
    const pe::SelectionOutline o = s.outline();
    PE_CHECK(o.complete);
    PE_CHECK(outlineIsRect(o.segments, pe::Rect{10, 20, 5, 7}));
}

PE_TEST(selection_outline_of_one_pixel_is_the_unit_square) {
    // Endpoints are pixel CORNERS. Getting that off by one draws the ants half a pixel
    // inside the selection at every zoom, which is exactly where it is least forgivable.
    pe::Selection s;
    s.selectRect(pe::Rect{0, 0, 1, 1});
    const pe::SelectionOutline o = s.outline();
    PE_CHECK(outlineIsRect(o.segments, pe::Rect{0, 0, 1, 1}));
}

PE_TEST(selection_outline_follows_the_shape_and_not_its_bounding_box) {
    // The defect this exists for. A freehand lasso committed a correct polygon mask and the
    // overlay drew tightBounds(), so the selection appeared to snap to a box the instant the
    // drag ended, and the pixels inside that box which were never selected then refused to
    // paint, which reads as the canvas not responding.
    pe::Selection s;
    const pe::Point tri[3] = {{0, 0}, {40, 0}, {0, 40}};
    s.selectPolygon(tri);
    PE_REQUIRE(s.active());

    const pe::SelectionOutline o = s.outline();
    PE_CHECK(o.complete);
    // A right triangle's hypotenuse is a staircase, so its outline is many segments, and
    // above all it is NOT the four of its bounding box.
    PE_CHECK(o.segments.size() > 4);
    PE_CHECK(!outlineIsRect(o.segments, s.tightBounds()));

    // Concretely: the box's right edge runs the full height, and the triangle's cannot.
    const pe::Rect b = s.tightBounds();
    const pe::OutlineSegment boxRight{pe::Point{b.right(), b.top()},
                                      pe::Point{b.right(), b.bottom()}};
    PE_CHECK(std::find(o.segments.begin(), o.segments.end(), boxRight) == o.segments.end());
}

PE_TEST(selection_outline_traces_holes_and_separate_islands) {
    // Both are shapes a wand produces routinely, and neither has a boundary a single
    // rectangle can describe.
    pe::Selection hole;
    hole.selectRect(pe::Rect{0, 0, 40, 40});
    hole.subtractRect(pe::Rect{10, 10, 10, 10});
    const pe::SelectionOutline ho = hole.outline();
    PE_CHECK(ho.complete);
    PE_CHECK_EQ(ho.segments.size(), static_cast<std::size_t>(8));  // outer four, inner four

    pe::Selection islands;
    islands.selectRect(pe::Rect{0, 0, 5, 5});
    islands.addRect(pe::Rect{20, 20, 5, 5});
    const pe::SelectionOutline io2 = islands.outline();
    PE_CHECK(io2.complete);
    PE_CHECK_EQ(io2.segments.size(), static_cast<std::size_t>(8));
}

PE_TEST(selection_outline_of_nothing_is_nothing) {
    pe::Selection none;
    PE_CHECK(none.outline().segments.empty());  // inactive: the whole document is editable
    PE_CHECK(none.outline().complete);

    pe::Selection cleared;
    cleared.selectRect(pe::Rect{0, 0, 4, 4});
    cleared.selectNone();
    PE_CHECK(cleared.outline().segments.empty());
}

PE_TEST(selection_outline_gives_up_rather_than_growing_without_bound) {
    // A ragged enough boundary has more segments than are worth drawing every frame. Giving
    // up has to be reported, because a boundary that stops halfway is a worse lie than the
    // bounding box this replaced, and the caller has to be able to say so.
    pe::Selection comb;
    for (int x = 0; x < 400; x += 2) comb.addRect(pe::Rect{x, 0, 1, 400});

    const pe::SelectionOutline full = comb.outline();
    PE_CHECK(full.complete);
    PE_CHECK(full.segments.size() > static_cast<std::size_t>(100));

    const pe::SelectionOutline capped = comb.outline(128, 10);
    PE_CHECK(!capped.complete);
    PE_CHECK(capped.segments.empty());  // nothing, rather than a partial outline
}

PE_TEST(selection_outline_traces_the_half_coverage_contour) {
    // A feathered selection has no single edge, so the outline draws the 50% contour: the
    // one contour that answers "which pixels are more selected than not".
    // Well inside the canvas on every side, so the feather can spread outward rather than
    // being clipped by the canvas edge, which is what makes the two extents differ at all.
    pe::Selection s;
    s.selectRect(pe::Rect{20, 20, 40, 40});
    s.feather(4.0f, pe::Rect{0, 0, 100, 100});
    PE_REQUIRE(s.active());

    const pe::Rect tight = s.tightBounds();
    PE_REQUIRE(tight.width > 40);  // feathering spread the non-zero coverage outward

    const pe::SelectionOutline o = s.outline();
    PE_CHECK(o.complete);
    PE_CHECK(!o.segments.empty());
    // The contour sits inside the non-zero extent, not on it: a feathered edge's outer
    // pixels are barely selected, and drawing the ants there would overstate the selection.
    int minX = std::numeric_limits<int>::max();
    for (const pe::OutlineSegment& seg : o.segments) {
        minX = std::min({minX, seg.a.x, seg.b.x});
    }
    PE_CHECK(minX > tight.left());
}
