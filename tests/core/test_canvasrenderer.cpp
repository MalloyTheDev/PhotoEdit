#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstdlib>
#include <memory>

using namespace pe;

namespace {
constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};
bool near8(Rgba8 a, Rgba8 b, int tol = 1) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol && d(a.a, b.a) <= tol;
}
}  // namespace

PE_TEST(renderer_first_render_composites_all_visible_tiles) {
    // 512x512 == a 2x2 tile grid.
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    PixelBuffer img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount(), static_cast<uint64_t>(4));
    PE_CHECK_EQ(img.at(0, 0), (Rgba8{0, 0, 0, 0}));  // empty base -> transparent
}

PE_TEST(renderer_recomposites_only_on_change) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());  // warm the cache (4)

    // Re-render with no change: zero recomposites (pure cache hit, like a pan).
    const uint64_t before = r.recompositeCount();
    (void)r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(0));
}

PE_TEST(renderer_invalidates_on_edit_and_reflects_it) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());  // cache 4 transparent tiles

    // Add a full-canvas red layer; the observer invalidates the affected tiles.
    auto layer = std::make_unique<SolidColorLayer>(kRed, doc->canvasBounds());
    const LayerId id = layer->id();
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));

    uint64_t before = r.recompositeCount();
    PixelBuffer img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(4));  // 4 dirty tiles
    PE_CHECK(near8(img.at(0, 0), kRed));
    PE_CHECK(near8(img.at(500, 500), kRed));

    // Opacity change dirties the layer's region -> recomposite those tiles only.
    before = r.recompositeCount();
    doc->history().push(std::make_unique<SetOpacityCommand>(id, 0.5f));
    img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(4));
    PE_CHECK(near8(img.at(0, 0), Rgba8{255, 0, 0, 128}));
}

PE_TEST(renderer_partial_invalidate_recomposites_one_tile) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());  // warm 4

    const uint64_t before = r.recompositeCount();
    r.invalidate(Rect{0, 0, 10, 10});  // touches tile (0,0) only
    (void)r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(1));
}

PE_TEST(renderer_lru_eviction_bounds_cache) {
    // 1024x1024 == a 4x4 (16) tile grid; with a 4-tile budget the cache stays
    // bounded even though all 16 are composited.
    auto doc = Document::createBlank(Size{1024, 1024});
    CanvasRenderer r(*doc);
    r.setCacheBudgetTiles(4);
    (void)r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount(), static_cast<uint64_t>(16));
    PE_CHECK(r.cachedTileCount() <= static_cast<std::size_t>(4));
}

PE_TEST(renderer_huge_invalidate_drops_cache) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());
    PE_CHECK(r.cachedTileCount() > static_cast<std::size_t>(0));
    // A document-spanning invalidate exceeds the per-call tile threshold and
    // drops the whole cache rather than growing the dirty set unbounded.
    r.invalidate(Rect{0, 0, 300000, 300000});
    PE_CHECK_EQ(r.cachedTileCount(), static_cast<std::size_t>(0));
}

// --- renderRegionScaled (display LOD for extreme zoom-out) ---

PE_TEST(renderer_scaled_small_region_matches_renderRegion) {
    // A region within the output cap must be byte-identical to renderRegion (no
    // regression / same fast path).
    auto doc = Document::createBlank(Size{512, 512});
    auto layer = std::make_unique<SolidColorLayer>(kRed, Rect{0, 0, 256, 512});  // left half red
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));

    CanvasRenderer r(*doc);
    PixelBuffer ref = r.renderRegion(doc->canvasBounds());
    PixelBuffer scaled = r.renderRegionScaled(doc->canvasBounds());
    PE_CHECK_EQ(scaled.width(), ref.width());
    PE_CHECK_EQ(scaled.height(), ref.height());
    bool identical = !ref.isEmpty() && !scaled.isEmpty();
    for (int y = 0; y < ref.height() && identical; y += 37) {
        for (int x = 0; x < ref.width() && identical; x += 37) {
            if (scaled.at(x, y) != ref.at(x, y)) identical = false;
        }
    }
    PE_CHECK(identical);
}

PE_TEST(renderer_scaled_over_budget_region_is_nonempty_and_bounded) {
    // 9000x9000 == 81 MP, above the 64 MP composite budget: renderRegion blanks,
    // but renderRegionScaled returns a bounded, correctly-sized downscale.
    auto doc = Document::createBlank(Size{9000, 9000});
    CanvasRenderer r(*doc);

    PE_CHECK(r.renderRegion(doc->canvasBounds()).isEmpty());  // the old behavior

    constexpr int kCap = 4'000'000;
    PixelBuffer img = r.renderRegionScaled(doc->canvasBounds(), kCap);
    PE_CHECK(!img.isEmpty());
    // Output area within the cap, and an integer downscale of the region.
    PE_CHECK(static_cast<int64_t>(img.width()) * img.height() <= kCap);
    PE_CHECK(img.width() > 0 && img.width() < 9000);
    PE_CHECK(img.height() > 0 && img.height() < 9000);
    // s = ceil(sqrt(81e6 / 4e6)) = 5  ->  ceil(9000/5) = 1800 per side.
    PE_CHECK_EQ(img.width(), 1800);
    PE_CHECK_EQ(img.height(), 1800);
}

PE_TEST(renderer_scaled_solid_canvas_downsamples_to_that_color) {
    // A solid-red huge canvas must downsample to (approximately) red everywhere.
    auto doc = Document::createBlank(Size{9000, 9000});
    auto layer = std::make_unique<SolidColorLayer>(kRed, doc->canvasBounds());
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));

    CanvasRenderer r(*doc);
    PixelBuffer img = r.renderRegionScaled(doc->canvasBounds());
    PE_CHECK(!img.isEmpty());
    PE_CHECK(near8(img.at(0, 0), kRed));
    PE_CHECK(near8(img.at(img.width() / 2, img.height() / 2), kRed));
    PE_CHECK(near8(img.at(img.width() - 1, img.height() - 1), kRed));
}

PE_TEST(renderer_scaled_half_canvas_shows_both_colors) {
    // Red left half, blue right half of a 9000x9000 over-budget canvas. The downscale
    // must preserve both colors in the correct halves. The 4500 split is a multiple
    // of the downscale factor (s=5), so no bin straddles the seam.
    auto doc = Document::createBlank(Size{9000, 9000});
    auto red = std::make_unique<SolidColorLayer>(kRed, Rect{0, 0, 4500, 9000});
    auto blue = std::make_unique<SolidColorLayer>(kBlue, Rect{4500, 0, 4500, 9000});
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(red), doc->topLevelCount()));
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(blue), doc->topLevelCount()));

    CanvasRenderer r(*doc);
    PixelBuffer img = r.renderRegionScaled(doc->canvasBounds(), 4'000'000);
    PE_CHECK(!img.isEmpty());
    const int mid = img.width() / 2;  // == 900, the red/blue seam at s=5
    PE_CHECK(near8(img.at(0, img.height() / 2), kRed));
    PE_CHECK(near8(img.at(mid / 2, img.height() / 2), kRed));
    PE_CHECK(near8(img.at(mid + mid / 2, img.height() / 2), kBlue));
    PE_CHECK(near8(img.at(img.width() - 1, img.height() / 2), kBlue));
}

PE_TEST(renderer_scaled_rejects_degenerate_inputs) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    PE_CHECK(r.renderRegionScaled(Rect{}).isEmpty());                   // empty region
    PE_CHECK(r.renderRegionScaled(doc->canvasBounds(), 0).isEmpty());   // non-positive cap
    PE_CHECK(r.renderRegionScaled(doc->canvasBounds(), -1).isEmpty());  // negative cap
    PE_CHECK(r.renderRegionScaled(Rect{0, 0, kMaxCanvasDimension + 1, 1}).isEmpty());  // too wide
}

PE_TEST(renderer_scaled_large_downscale_preserves_opacity) {
    // Audit regression: at an extreme downscale a bin sums >2^24 unit-magnitude samples; a float32
    // accumulator saturates and collapses the averaged ALPHA toward 0 (opaque content rendered
    // semi-transparent). Double accumulators keep it exact. 9000x9000 solid red, cap=4 -> s=4500,
    // ~20M samples/bin (> 2^24).
    auto doc = Document::createBlank(Size{9000, 9000});
    auto layer = std::make_unique<SolidColorLayer>(kRed, doc->canvasBounds());
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));
    CanvasRenderer r(*doc);
    PixelBuffer img = r.renderRegionScaled(doc->canvasBounds(), 4);  // tiny cap forces s=4500
    PE_CHECK(!img.isEmpty());
    PE_CHECK(near8(img.at(0, 0), kRed));  // color preserved
    PE_CHECK_EQ(static_cast<int>(img.at(0, 0).a),
                255);  // alpha NOT collapsed (would be ~211 in f32)
}

PE_TEST(renderer_scaled_caps_source_tile_compute) {
    // Audit regression: the scaled path drops renderRegion's area cap (to allow zoom-out) but must
    // still bound COMPUTE — a near-max-extent region would composite millions of tiles. Beyond the
    // source-tile cap it degrades to empty rather than freezing.
    auto doc = Document::createBlank(Size{40000, 40000});  // 157x157 = 24649 source tiles
    CanvasRenderer r(*doc);
    PE_CHECK(r.renderRegionScaled(doc->canvasBounds()).isEmpty());  // over the tile-compute cap
    // A smaller over-budget region (within the tile cap) still renders.
    PE_CHECK(!r.renderRegionScaled(Rect{0, 0, 9000, 9000}, 4'000'000).isEmpty());
}

PE_TEST(renderer_undo_restores_pixels) {
    auto doc = Document::createBlank(Size{256, 256});  // single tile
    CanvasRenderer r(*doc);
    auto layer = std::make_unique<SolidColorLayer>(kRed, doc->canvasBounds());
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));
    PixelBuffer img = r.renderRegion(doc->canvasBounds());
    PE_CHECK(near8(img.at(0, 0), kRed));

    doc->history().undo();  // remove the red layer
    img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(img.at(0, 0), (Rgba8{0, 0, 0, 0}));  // back to transparent
}
