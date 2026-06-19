#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PaintToolController.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace pe;

namespace {
constexpr Rgbaf kRedF{1.0f, 0.0f, 0.0f, 1.0f};

int alphaAt(const Document& doc, LayerId id, int x, int y) {
    const Layer* l = doc.findLayer(id);
    const auto* pl = static_cast<const PixelLayer*>(l);
    return pl->tiles().pixel(x, y).a;
}

BrushSettings hardBrush(float diameter, float opacity) {
    BrushSettings b;
    b.diameter = diameter;
    b.hardness = 1.0f;
    b.opacity = opacity;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    return b;
}

// Fill a 64x64 doc with a black|white edge at x=32, then gaussian-blur the whole layer so the edge
// becomes a smooth ramp with genuine curvature on both sides — a "soft edge" for the Sharpen brush
// to crisp back up. Leaves the doc with empty history (the setup blur is not pushed).
void makeSoftEdge(Document& doc) {
    const LayerId base = doc.activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc.findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 32, 64}, Rgba8{0, 0, 0, 255});
    pl->tiles().fillRect(Rect{32, 0, 32, 64}, Rgba8{255, 255, 255, 255});
    auto soft = applyFilter(doc, base, GaussianBlurFilter(3.0f));
    soft->execute(doc);  // apply directly; do NOT push, so each test starts with an empty history
}
}  // namespace

PE_TEST(dab_coverage_table) {
    PE_CHECK_NEAR(dabCoverage(0.0f, 1.0f), 1.0f);   // center solid
    PE_CHECK_NEAR(dabCoverage(0.99f, 1.0f), 1.0f);  // inside crisp tip
    PE_CHECK_NEAR(dabCoverage(1.0f, 1.0f), 0.0f);   // rim
    PE_CHECK_NEAR(dabCoverage(0.0f, 0.0f), 1.0f);   // soft tip center
    PE_CHECK_NEAR(dabCoverage(0.5f, 0.0f), 0.5f);   // smoothstep midpoint
    PE_CHECK_NEAR(dabCoverage(0.5f, 0.5f), 1.0f);   // d<=hardness solid
    PE_CHECK_NEAR(dabCoverage(0.75f, 0.5f), 0.5f);  // shoulder midpoint
    PE_CHECK_NEAR(dabCoverage(2.0f, 0.5f), 0.0f);   // outside
}

PE_TEST(paint_stroke_deposits_and_undo_restores) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{8, 32}, 1.0f}, {{56, 32}, 1.0f}};

    auto cmd = paintStroke(*doc, base, hardBrush(12, 1.0f), kRedF, pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    // Pixels along the stroke centerline are painted opaque red.
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);
    PE_CHECK_EQ(doc->findLayer(base)->contentBounds().isEmpty(), false);

    doc->history().undo();
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 0);  // restored transparent
    PE_CHECK(doc->findLayer(base)->contentBounds().isEmpty());

    doc->history().redo();
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // repainted
}

PE_TEST(paint_single_click_one_dab) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};  // a single click
    auto cmd = paintStroke(*doc, base, hardBrush(16, 1.0f), kRedF, pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // dab landed at the click
}

PE_TEST(opacity_no_double_darken_within_stroke) {
    // A stroke that crosses itself at 50% opacity must NOT exceed 50% at the
    // crossing (stroke-buffer cap). Center alpha ~= 128.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{16, 32}, 1.0f}, {{48, 32}, 1.0f}, {{16, 32}, 1.0f}};
    auto cmd = paintStroke(*doc, base, hardBrush(14, 0.5f), kRedF, pts);
    doc->history().push(std::move(cmd));
    const int a = alphaAt(*doc, base, 32, 32);
    PE_CHECK(a >= 126 && a <= 130);  // ~128, not doubled
}

PE_TEST(separate_strokes_stack) {
    // Two SEPARATE 50% strokes over the same spot stack toward 75% (~191).
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    doc->history().push(paintStroke(*doc, base, hardBrush(16, 0.5f), kRedF, pts));
    doc->history().push(paintStroke(*doc, base, hardBrush(16, 0.5f), kRedF, pts));
    const int a = alphaAt(*doc, base, 32, 32);
    PE_CHECK(a >= 189 && a <= 193);  // ~191
}

PE_TEST(paint_tile_delta_is_local) {
    // A small stroke on a large canvas touches only a few tiles (not the whole
    // document) — the tile-delta undo memory scales with painted area.
    auto doc = Document::createBlank(Size{2048, 2048});  // 8x8 = 64 tiles
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{40, 40}, 1.0f}, {{80, 40}, 1.0f}};
    auto cmd = paintStroke(*doc, base, hardBrush(10, 1.0f), kRedF, pts);
    PE_CHECK(cmd != nullptr);
    PE_CHECK(cmd->touchedTileCount() <= static_cast<std::size_t>(2));
}

PE_TEST(erase_reduces_alpha) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    doc->history().push(paintStroke(*doc, base, hardBrush(20, 1.0f), kRedF, pts));
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);

    doc->history().push(eraseStroke(*doc, base, hardBrush(20, 1.0f), pts));
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 0);  // fully erased at center

    doc->history().undo();                          // undo erase
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // red restored
}

PE_TEST(paint_on_missing_layer_is_null) {
    auto doc = Document::createBlank(Size{64, 64});
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    PE_CHECK(paintStroke(*doc, 999999, hardBrush(16, 1.0f), kRedF, pts) == nullptr);
}

PE_TEST(paint_nonfinite_points_are_safe) {
    // NaN/Inf input samples (garbage tablet data) must not crash or deposit
    // anything (no UB in the float->int casts).
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    const float nan = std::nan("");
    std::vector<StrokePoint> pts = {{{nan, 32.0f}, 1.0f}, {{INFINITY, 32.0f}, 1.0f}};
    PE_CHECK(paintStroke(*doc, base, hardBrush(16, 1.0f), kRedF, pts) == nullptr);
}

PE_TEST(paint_absurd_coordinates_bounded) {
    // A finite but absurd coordinate is skipped (beyond the off-canvas bound) —
    // no gigabyte allocation, no overflow.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{1e30f, 1e30f}, 1.0f}};
    PE_CHECK(paintStroke(*doc, base, hardBrush(16, 1.0f), kRedF, pts) == nullptr);
}

PE_TEST(erase_empty_space_is_null) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    PE_CHECK(eraseStroke(*doc, base, hardBrush(16, 1.0f), pts) == nullptr);  // nothing to erase
}

PE_TEST(paint_stabilize_smooths_without_collapsing) {
    // Stabilization (BrushSettings::stabilize) low-pass-filters the path. With many
    // samples and a moderate factor the stroke must still reach the end of the path
    // (it must NOT collapse onto the first point — the bug when the lag factor is 1).
    auto doc = Document::createBlank(Size{96, 96});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 40; ++i) {
        pts.push_back(StrokePoint{{8.0f + static_cast<float>(i) * 2.0f, 48.0f}, 1.0f});
    }
    BrushSettings b = hardBrush(10, 1.0f);
    b.stabilize = 0.5f;
    auto cmd = paintStroke(*doc, base, b, kRedF, pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(alphaAt(*doc, base, 8, 48) > 0);   // painted near the start
    PE_CHECK(alphaAt(*doc, base, 80, 48) > 0);  // and the lag catches up to the end

    // An extreme factor of 1.0 must be clamped internally so it still paints a
    // span, not a single dab at the origin.
    auto doc2 = Document::createBlank(Size{96, 96});
    BrushSettings b2 = hardBrush(10, 1.0f);
    b2.stabilize = 1.0f;
    auto cmd2 = paintStroke(*doc2, doc2->activeLayer(), b2, kRedF, pts);
    PE_CHECK(cmd2 != nullptr);  // does not collapse to nothing / crash
}

PE_TEST(dodge_lightens_burn_darkens_and_undo) {
    // A mid-gray opaque field: a dodge stroke through the center lightens it; burn darkens it.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{128, 128, 128, 255});
    const Rgba8 before = pl->tiles().pixel(32, 32);

    std::vector<StrokePoint> pts = {{{8, 32}, 1.0f}, {{56, 32}, 1.0f}};
    auto dodge = dodgeStroke(*doc, base, hardBrush(12, 1.0f), pts);
    PE_CHECK(dodge != nullptr);
    doc->history().push(std::move(dodge));
    const Rgba8 dodged = pl->tiles().pixel(32, 32);
    PE_CHECK(dodged.r > before.r);                 // lightened
    PE_CHECK_EQ(dodged.a, before.a);               // alpha preserved
    PE_CHECK_EQ(pl->tiles().pixel(2, 2), before);  // off-stroke pixel unchanged

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(32, 32), before);  // restored

    auto burn = burnStroke(*doc, base, hardBrush(12, 1.0f), pts);
    PE_CHECK(burn != nullptr);
    doc->history().push(std::move(burn));
    const Rgba8 burned = pl->tiles().pixel(32, 32);
    PE_CHECK(burned.r < before.r);    // darkened
    PE_CHECK_EQ(burned.a, before.a);  // alpha preserved
}

PE_TEST(dodge_burn_skip_transparent_pixels) {
    // On a fully transparent layer the tone tools change nothing (no phantom RGB deltas), so
    // the stroke deposits no command.
    auto doc = Document::createBlank(Size{32, 32});
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{4, 16}, 1.0f}, {{28, 16}, 1.0f}};
    PE_CHECK(dodgeStroke(*doc, base, hardBrush(12, 1.0f), pts) == nullptr);
    PE_CHECK(burnStroke(*doc, base, hardBrush(12, 1.0f), pts) == nullptr);
}

PE_TEST(controller_dodge_and_burn_modes) {
    // Drive the tone ops through PaintToolController (the path the Dodge tool + Alt=Burn use).
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{128, 128, 128, 255});
    const int before = pl->tiles().pixel(32, 32).r;

    PaintToolController c;
    c.setBrush(hardBrush(16, 1.0f));
    c.setMode(PaintToolController::Mode::Dodge);
    PE_CHECK(c.begin(*doc, StrokePoint{{32, 32}, 1.0f}));
    PE_CHECK(c.end(*doc));
    PE_CHECK(pl->tiles().pixel(32, 32).r > before);  // dodge lightened
    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(32, 32).r, before);

    c.setMode(PaintToolController::Mode::Burn);
    PE_CHECK(c.begin(*doc, StrokePoint{{32, 32}, 1.0f}));
    PE_CHECK(c.end(*doc));
    PE_CHECK(pl->tiles().pixel(32, 32).r < before);  // burn darkened
}

PE_TEST(dodge_honors_selection) {
    auto doc = Document::createBlank(Size{32, 32});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{128, 128, 128, 255});

    Selection sel;
    sel.selectRect(Rect{0, 0, 16, 32});                                  // left half selected
    std::vector<StrokePoint> pts = {{{4, 16}, 1.0f}, {{28, 16}, 1.0f}};  // spans both halves
    auto cmd = dodgeStroke(*doc, base, hardBrush(16, 1.0f), pts, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(pl->tiles().pixel(6, 16).r > 128);     // inside selection -> lightened
    PE_CHECK_EQ(pl->tiles().pixel(26, 16).r, 128);  // outside selection -> unchanged
}

PE_TEST(dodge_on_16bit_layer) {
    auto doc = Document::createBlank(Size{32, 32}, ColorMode::RGB, BitDepth::U16);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles16().fillRect(Rect{0, 0, 32, 32}, Rgba16{32768, 32768, 32768, 65535});  // mid-gray
    const int before = pl->tiles16().pixel(16, 16).r;
    std::vector<StrokePoint> pts = {{{4, 16}, 1.0f}, {{28, 16}, 1.0f}};
    auto cmd = dodgeStroke(*doc, base, hardBrush(16, 1.0f), pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(pl->tiles16().pixel(16, 16).r > before);  // lightened in the native 16-bit store
}

PE_TEST(dodge_does_not_darken_superwhite_f32) {
    // On an F32 layer holding a super-white (>1.0) value, dodge must NOT reduce it (the
    // max(0, 1-dst) guard) — a "lighten" tool must never darken.
    auto doc = Document::createBlank(Size{16, 16}, ColorMode::RGB, BitDepth::F32);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tilesF().fillRect(Rect{0, 0, 16, 16}, Rgbaf{2.0f, 2.0f, 2.0f, 1.0f});  // HDR highlight
    std::vector<StrokePoint> pts = {{{2, 8}, 1.0f}, {{14, 8}, 1.0f}};
    auto cmd = dodgeStroke(*doc, base, hardBrush(12, 1.0f), pts);
    // With the clamp, dodging a super-white pixel is a no-op, so the stroke may deposit nothing.
    if (cmd != nullptr) doc->history().push(std::move(cmd));
    PE_CHECK(pl->tilesF().pixel(8, 8).r >= 2.0f - 1e-4f);  // not darkened
}

PE_TEST(clone_stroke_copies_source_region) {
    // A red square at the top-left; cloning with an offset that maps the brushed area back onto the
    // red square copies red into the (empty) brushed area.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{255, 0, 0, 255});  // red source patch

    // Paint at (40,40); offset = firstPoint - sourceAnchor = (40,40) - (8,8) = (32,32). So pixel
    // (40,40) samples source (8,8) which is inside the red patch.
    std::vector<StrokePoint> pts = {{{40, 40}, 1.0f}};
    auto cmd = cloneStroke(*doc, base, hardBrush(8, 1.0f), pts, /*offsetX=*/32, /*offsetY=*/32);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(40, 40), (Rgba8{255, 0, 0, 255}));  // red cloned in
    PE_CHECK(pl->tiles().pixel(0, 0).r == 255);                       // source untouched

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(40, 40).a, static_cast<uint8_t>(0));  // restored
}

PE_TEST(clone_stroke_empty_source_deposits_nothing) {
    // Cloning from an empty (transparent) source region composites nothing -> no command.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{255, 0, 0, 255});  // red patch at top-left
    // Offset (0,0): pixel (40,40) samples source (40,40), which is transparent.
    std::vector<StrokePoint> pts = {{{40, 40}, 1.0f}};
    PE_CHECK(cloneStroke(*doc, base, hardBrush(8, 1.0f), pts, 0, 0) == nullptr);
}

PE_TEST(controller_clone_no_feedback_on_overlapping_drag) {
    // Drive Clone through the controller (begin/extend/end). A left->right drag with a small
    // offset over a narrow source stripe must NOT smear: each painted pixel samples the PRE-STROKE
    // (S0) source, so red only appears where the original source maps in — it does not propagate
    // across the whole drag (which is what a feedback bug would do).
    auto doc = Document::createBlank(Size{80, 80});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 16, 80}, Rgba8{255, 0, 0, 255});  // 16px red stripe on the left

    PaintToolController c;
    c.setBrush(hardBrush(8, 1.0f));
    c.setMode(PaintToolController::Mode::Clone);
    c.setCloneSource(Point{8, 40});                        // inside the red stripe
    PE_CHECK(c.begin(*doc, StrokePoint{{18, 40}, 1.0f}));  // offset = (18-8, 0) = (10, 0)
    c.extend(*doc, StrokePoint{{30, 40}, 1.0f});
    c.extend(*doc, StrokePoint{{50, 40}, 1.0f});
    c.extend(*doc, StrokePoint{{70, 40}, 1.0f});
    PE_CHECK(c.end(*doc));

    PE_CHECK_EQ(pl->tiles().pixel(20, 40), (Rgba8{255, 0, 0, 255}));  // samples original x=10 (red)
    PE_CHECK_EQ(pl->tiles().pixel(50, 40).a,
                static_cast<uint8_t>(0));  // samples original x=40 (empty) -> no smear
}

PE_TEST(controller_clone_requires_a_source) {
    // Clone mode with no source anchor deposits nothing (the stroke commits no command).
    auto doc = Document::createBlank(Size{32, 32});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{0, 0, 255, 255});
    PaintToolController c;
    c.setBrush(hardBrush(8, 1.0f));
    c.setMode(PaintToolController::Mode::Clone);  // no setCloneSource()
    PE_CHECK(c.begin(*doc, StrokePoint{{16, 16}, 1.0f}));
    PE_CHECK(!c.end(*doc));  // nothing deposited
}

PE_TEST(clone_honors_selection) {
    auto doc = Document::createBlank(Size{32, 32});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 8, 32}, Rgba8{255, 0, 0, 255});  // red source strip

    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 8});  // only the top band is writable
    std::vector<StrokePoint> pts = {{{20, 4}, 1.0f},
                                    {{20, 20}, 1.0f}};  // spans in/out of selection
    auto cmd =
        cloneStroke(*doc, base, hardBrush(8, 1.0f), pts, /*offsetX=*/16, /*offsetY=*/0, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(20, 4), (Rgba8{255, 0, 0, 255}));  // in selection -> cloned
    PE_CHECK_EQ(pl->tiles().pixel(20, 20).a,
                static_cast<uint8_t>(0));  // outside selection -> untouched
}

PE_TEST(clone_on_16bit_layer) {
    auto doc = Document::createBlank(Size{32, 32}, ColorMode::RGB, BitDepth::U16);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles16().fillRect(Rect{0, 0, 8, 32}, Rgba16{60000, 0, 0, 65535});  // red source (16-bit)
    std::vector<StrokePoint> pts = {{{20, 16}, 1.0f}};
    auto cmd = cloneStroke(*doc, base, hardBrush(8, 1.0f), pts, /*offsetX=*/16, /*offsetY=*/0);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const Rgba16 px = pl->tiles16().pixel(20, 16);
    PE_CHECK(px.r > 55000 && px.g < 2000 && px.a > 60000);  // cloned 16-bit red
}

PE_TEST(blur_softens_hard_edge_and_undo_restores) {
    // A hard black|white vertical edge at x=32. A blur stroke across it must soften the edge:
    // pixels right at the boundary pick up intermediate gray values that did not exist before.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 32, 64}, Rgba8{0, 0, 0, 255});         // left half black
    pl->tiles().fillRect(Rect{32, 0, 32, 64}, Rgba8{255, 255, 255, 255});  // right half white

    // Before: every pixel is pure black or pure white (a hard edge, no intermediate values).
    PE_CHECK_EQ(pl->tiles().pixel(31, 32).r, static_cast<uint8_t>(0));
    PE_CHECK_EQ(pl->tiles().pixel(32, 32).r, static_cast<uint8_t>(255));
    const Rgba8 farLeft = pl->tiles().pixel(2, 32);  // well outside the brushed band

    std::vector<StrokePoint> pts = {{{32, 8}, 1.0f}, {{32, 56}, 1.0f}};  // drag down the edge
    auto cmd = blurStroke(*doc, base, hardBrush(20, 1.0f), pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    // After: a pixel just left of the edge is lightened above black, just right is darkened below
    // white — i.e. intermediate gray appeared where the edge used to be crisp.
    const int leftR = pl->tiles().pixel(31, 32).r;
    const int rightR = pl->tiles().pixel(32, 32).r;
    PE_CHECK(leftR > 0 && leftR < 255);
    PE_CHECK(rightR > 0 && rightR < 255);
    PE_CHECK_EQ(pl->tiles().pixel(2, 32), farLeft);  // far from the stroke: unchanged

    doc->history().undo();  // exact restore: the edge is hard again
    PE_CHECK_EQ(pl->tiles().pixel(31, 32).r, static_cast<uint8_t>(0));
    PE_CHECK_EQ(pl->tiles().pixel(32, 32).r, static_cast<uint8_t>(255));
}

PE_TEST(blur_honors_selection) {
    // A hard black|white edge with only the left half selected. A blur stroke across the edge must
    // soften only inside the selection; pixels outside it stay exactly black or white.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 32, 64}, Rgba8{0, 0, 0, 255});
    pl->tiles().fillRect(Rect{32, 0, 32, 64}, Rgba8{255, 255, 255, 255});

    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 64});  // only the left (black) half is writable
    std::vector<StrokePoint> pts = {{{32, 8}, 1.0f}, {{32, 56}, 1.0f}};
    auto cmd = blurStroke(*doc, base, hardBrush(20, 1.0f), pts, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK(pl->tiles().pixel(31, 32).r > 0);  // inside selection -> softened
    PE_CHECK_EQ(pl->tiles().pixel(40, 32).r,
                static_cast<uint8_t>(255));  // outside -> untouched white
}

PE_TEST(blur_empty_stroke_deposits_nothing) {
    // A blur stroke whose path is empty (or off-canvas) has no coverage, so no command is produced.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{128, 128, 128, 255});

    std::vector<StrokePoint> empty;
    PE_CHECK(blurStroke(*doc, base, hardBrush(16, 1.0f), empty) == nullptr);

    // A finite-but-absurd off-canvas point also yields no coverage -> nullptr (no allocation
    // blowup).
    std::vector<StrokePoint> absurd = {{{1e30f, 1e30f}, 1.0f}};
    PE_CHECK(blurStroke(*doc, base, hardBrush(16, 1.0f), absurd) == nullptr);
}

PE_TEST(controller_blur_mode_softens_edge) {
    // Drive Blur through PaintToolController (the path the Blur tool uses): begin/end across a hard
    // edge softens it, and it commits exactly one undoable command.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 32, 64}, Rgba8{0, 0, 0, 255});
    pl->tiles().fillRect(Rect{32, 0, 32, 64}, Rgba8{255, 255, 255, 255});

    PaintToolController c;
    c.setBrush(hardBrush(24, 1.0f));
    c.setMode(PaintToolController::Mode::Blur);
    PE_CHECK(c.begin(*doc, StrokePoint{{32, 32}, 1.0f}));
    PE_CHECK(c.end(*doc));
    const int leftR = pl->tiles().pixel(31, 32).r;
    PE_CHECK(leftR > 0 && leftR < 255);  // softened at the edge

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(31, 32).r, static_cast<uint8_t>(0));  // one undo step restores it
}

PE_TEST(sharpen_increases_contrast_and_undo_restores) {
    // A soft (gaussian-blurred) black|white edge at x=32. A Sharpen stroke across it must increase
    // local contrast: the darker side near the edge gets darker and the lighter side gets lighter
    // (the unsharp overshoot), so the edge is crisper than the smooth ramp it started from.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    makeSoftEdge(*doc);

    const Rgba8 darkBefore = pl->tiles().pixel(30, 32);   // soft edge, just left of center
    const Rgba8 lightBefore = pl->tiles().pixel(34, 32);  // soft edge, just right of center
    const Rgba8 farLeft = pl->tiles().pixel(2, 32);       // well outside the brushed band
    PE_CHECK(darkBefore.r > 0 && darkBefore.r < 255);     // genuinely soft, not pure black/white
    PE_CHECK(lightBefore.r > 0 && lightBefore.r < 255);

    std::vector<StrokePoint> pts = {{{32, 8}, 1.0f}, {{32, 56}, 1.0f}};  // drag down the edge
    auto cmd = sharpenStroke(*doc, base, hardBrush(20, 1.0f), pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    // After: contrast across the edge widened — the darker side dropped, the lighter side rose.
    PE_CHECK(pl->tiles().pixel(30, 32).r < darkBefore.r);
    PE_CHECK(pl->tiles().pixel(34, 32).r > lightBefore.r);
    PE_CHECK_EQ(pl->tiles().pixel(2, 32), farLeft);  // far from the stroke: unchanged

    doc->history().undo();  // exact restore: the soft edge returns
    PE_CHECK_EQ(pl->tiles().pixel(30, 32), darkBefore);
    PE_CHECK_EQ(pl->tiles().pixel(34, 32), lightBefore);
}

PE_TEST(sharpen_honors_selection) {
    // The same soft edge with only the left half selected. A sharpen stroke across the edge must
    // affect only inside the selection; pixels outside it keep their exact value.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    makeSoftEdge(*doc);

    const Rgba8 insideBefore = pl->tiles().pixel(30, 32);
    const Rgba8 outsideBefore = pl->tiles().pixel(34, 32);

    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 64});  // only the left half is writable
    std::vector<StrokePoint> pts = {{{32, 8}, 1.0f}, {{32, 56}, 1.0f}};
    auto cmd = sharpenStroke(*doc, base, hardBrush(20, 1.0f), pts, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK(pl->tiles().pixel(30, 32).r < insideBefore.r);  // inside selection -> sharpened
    PE_CHECK_EQ(pl->tiles().pixel(34, 32), outsideBefore);   // outside selection -> untouched
}

PE_TEST(sharpen_empty_stroke_deposits_nothing) {
    // A sharpen stroke whose path is empty (or off-canvas) has no coverage, so no command results.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{128, 128, 128, 255});

    std::vector<StrokePoint> empty;
    PE_CHECK(sharpenStroke(*doc, base, hardBrush(16, 1.0f), empty) == nullptr);

    // A finite-but-absurd off-canvas point also yields no coverage -> nullptr (no allocation
    // blowup).
    std::vector<StrokePoint> absurd = {{{1e30f, 1e30f}, 1.0f}};
    PE_CHECK(sharpenStroke(*doc, base, hardBrush(16, 1.0f), absurd) == nullptr);
}

PE_TEST(controller_sharpen_mode_increases_contrast) {
    // Drive Sharpen through PaintToolController (the path the Blur tool uses with Alt): begin/end
    // over a soft edge crisps it, and it commits exactly one undoable command.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    makeSoftEdge(*doc);

    const Rgba8 darkBefore = pl->tiles().pixel(30, 32);

    PaintToolController c;
    c.setBrush(hardBrush(24, 1.0f));
    c.setMode(PaintToolController::Mode::Sharpen);
    PE_CHECK(c.begin(*doc, StrokePoint{{32, 32}, 1.0f}));
    PE_CHECK(c.end(*doc));
    PE_CHECK(pl->tiles().pixel(30, 32).r < darkBefore.r);  // local contrast boosted at the edge

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(30, 32), darkBefore);  // one undo step restores it
}

PE_TEST(clone_preserves_superwhite_on_f32) {
    // Cloning an HDR (>1.0) source onto a transparent F32 layer must reproduce the value, not clip
    // it to 1.0 (Clone uses an unclamped straight-alpha composite for Normal blend).
    auto doc = Document::createBlank(Size{32, 32}, ColorMode::RGB, BitDepth::F32);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tilesF().fillRect(Rect{0, 0, 8, 32}, Rgbaf{4.0f, 0.0f, 0.0f, 1.0f});  // super-white red
    std::vector<StrokePoint> pts = {{{20, 16}, 1.0f}};
    auto cmd = cloneStroke(*doc, base, hardBrush(8, 1.0f), pts, /*offsetX=*/16, /*offsetY=*/0);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(pl->tilesF().pixel(20, 16).r > 3.5f);  // HDR value preserved, not clamped to 1.0
}
