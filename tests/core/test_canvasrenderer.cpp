#include <cmath>
#include <cstdint>
#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
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

PE_TEST(renderer_rejects_far_offset_origin_without_overflow) {
    // A far-offset rect passes the width/height/area caps but x+width (Rect::right) and
    // col*kTileSize (tilesForRect) would signed-overflow int = UB. Both render methods must reject
    // it safely.
    auto doc = Document::createBlank(Size{256, 256});
    CanvasRenderer r(*doc);
    const Rect farX{2'000'000'000, 0, 64, 64};  // x near INT_MAX, small extent
    const Rect farY{0, 2'000'000'000, 64, 64};
    PE_CHECK(r.renderRegion(farX).isEmpty());
    PE_CHECK(r.renderRegion(farY).isEmpty());
    PE_CHECK(r.renderRegion(Rect{-2'000'000'000, 0, 64, 64}).isEmpty());
    PE_CHECK(r.renderRegionScaled(farX, 4096).isEmpty());
    PE_CHECK(r.renderRegionScaled(farY, 4096).isEmpty());
    // A normal region still renders (guard didn't over-reject).
    PE_CHECK(!r.renderRegion(doc->canvasBounds()).isEmpty());
}

namespace {

// A document several tiles across with per-tile colour, so a wrong tile, a wrong block
// offset or a stale block is visible rather than averaged away. Per the standing rule,
// every case here spans more than one tile.
std::unique_ptr<Document> tiledDoc(int cols, int rows) {
    auto doc = Document::createBlank(Size{cols * kTileSize, rows * kTileSize});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            pl->tiles().fillRect(Rect{c * kTileSize, r * kTileSize, kTileSize, kTileSize},
                                 Rgba8{static_cast<std::uint8_t>(20 + 40 * c),
                                       static_cast<std::uint8_t>(20 + 40 * r), 180, 255});
        }
    }
    return doc;
}

bool sameBuffer(const PixelBuffer& a, const PixelBuffer& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!(a.at(x, y) == b.at(x, y))) return false;
        }
    }
    return true;
}

}  // namespace

PE_TEST(scaledcache_incremental_update_equals_a_full_rebuild) {
    // The property the whole design rests on: patching only the tiles that changed must
    // land on exactly what rebuilding everything would. If it does not, a zoomed-out view
    // shows stale pixels that no repaint corrects, which is worse than being slow.
    auto doc = tiledDoc(4, 3);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    const Rect all{0, 0, 4 * kTileSize, 3 * kTileSize};
    const int cap = 4096;  // forces a real downscale over this region

    CanvasRenderer r(*doc);
    Rect cov{};
    (void)r.renderRegionScaledCached(all, cap, cov);

    // Change two tiles, one of them not adjacent to the other.
    pl->tiles().fillRect(Rect{kTileSize + 10, 10, 60, 60}, Rgba8{255, 0, 0, 255});
    r.invalidate(Rect{kTileSize + 10, 10, 60, 60});
    pl->tiles().fillRect(Rect{3 * kTileSize + 5, 2 * kTileSize + 5, 40, 40}, Rgba8{0, 255, 0, 255});
    r.invalidate(Rect{3 * kTileSize + 5, 2 * kTileSize + 5, 40, 40});

    const PixelBuffer patched = r.renderRegionScaledCached(all, cap, cov);  // incremental

    CanvasRenderer fresh(*doc);  // same document, nothing cached
    Rect cov2{};
    const PixelBuffer rebuilt = fresh.renderRegionScaledCached(all, cap, cov2);

    PE_CHECK(cov == cov2);
    PE_CHECK(sameBuffer(patched, rebuilt));
}

PE_TEST(scaledcache_covers_the_tile_aligned_region_it_reports) {
    // The buffer covers a region grown to whole tiles, and the caller has to draw it to
    // THAT rect. Reporting one rect and covering another would shift the whole image.
    auto doc = tiledDoc(3, 2);
    CanvasRenderer r(*doc);
    Rect cov{};
    // A request deliberately off the tile grid.
    const Rect want{37, 91, 2 * kTileSize, kTileSize + 40};
    const PixelBuffer buf = r.renderRegionScaledCached(want, 4096, cov);

    PE_CHECK(cov.x % kTileSize == 0);
    PE_CHECK(cov.y % kTileSize == 0);
    PE_CHECK(cov.width % kTileSize == 0);
    PE_CHECK(cov.height % kTileSize == 0);
    PE_CHECK(cov.left() <= want.left() && cov.top() <= want.top());
    PE_CHECK(cov.right() >= want.right() && cov.bottom() >= want.bottom());
    PE_CHECK(!buf.isEmpty());
    // The buffer's own extent has to agree with the region and the scale it chose, or the
    // stretch the caller applies is wrong.
    PE_CHECK_EQ(cov.width % buf.width(), 0);
    PE_CHECK_EQ(cov.height % buf.height(), 0);
    PE_CHECK_EQ(cov.width / buf.width(), cov.height / buf.height());
}

PE_TEST(scaledcache_reuses_the_buffer_when_nothing_changed) {
    // The point of retaining it: a repaint that changes neither the region nor the document
    // must not recomposite. recompositeCount is the engine's own counter, so this measures
    // work done rather than wall clock.
    auto doc = tiledDoc(4, 3);
    const Rect all{0, 0, 4 * kTileSize, 3 * kTileSize};
    CanvasRenderer r(*doc);
    Rect cov{};
    (void)r.renderRegionScaledCached(all, 4096, cov);
    const std::uint64_t afterFirst = r.recompositeCount();
    PE_CHECK(afterFirst > 0);

    for (int i = 0; i < 5; ++i) (void)r.renderRegionScaledCached(all, 4096, cov);
    PE_CHECK_EQ(r.recompositeCount(), afterFirst);  // five repaints, no work
}

PE_TEST(scaledcache_repaints_only_the_tiles_that_changed) {
    // And the interactive property: one dab must cost one tile, not the visible span.
    auto doc = tiledDoc(4, 3);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    const Rect all{0, 0, 4 * kTileSize, 3 * kTileSize};
    CanvasRenderer r(*doc);
    Rect cov{};
    (void)r.renderRegionScaledCached(all, 4096, cov);
    const std::uint64_t afterFirst = r.recompositeCount();

    pl->tiles().fillRect(Rect{20, 20, 8, 8}, Rgba8{255, 255, 0, 255});
    r.invalidate(Rect{20, 20, 8, 8});
    (void)r.renderRegionScaledCached(all, 4096, cov);

    // Exactly one tile recomposited, out of the twelve the region spans.
    PE_CHECK_EQ(r.recompositeCount() - afterFirst, static_cast<std::uint64_t>(1));
}

PE_TEST(scaledcache_rebuilds_when_the_region_or_scale_moves) {
    // Panning or zooming changes the shape, and a buffer built for one shape cannot be
    // patched into another. It has to rebuild rather than return the wrong pixels.
    auto doc = tiledDoc(4, 3);
    const Rect a{0, 0, 4 * kTileSize, 3 * kTileSize};
    const Rect b{kTileSize, 0, 3 * kTileSize, 3 * kTileSize};
    CanvasRenderer r(*doc);
    Rect cov{};
    const PixelBuffer first = r.renderRegionScaledCached(a, 4096, cov);
    const Rect covA = cov;
    const PixelBuffer second = r.renderRegionScaledCached(b, 4096, cov);
    PE_CHECK(!(cov == covA));

    CanvasRenderer fresh(*doc);
    Rect cov2{};
    const PixelBuffer freshB = fresh.renderRegionScaledCached(b, 4096, cov2);
    PE_CHECK(cov == cov2);
    PE_CHECK(sameBuffer(second, freshB));
}

PE_TEST(scaledcache_is_dropped_by_a_whole_document_invalidation) {
    // invalidateAll has no extent to patch from, so the retained buffer must go rather than
    // be served stale.
    auto doc = tiledDoc(3, 2);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    const Rect all{0, 0, 3 * kTileSize, 2 * kTileSize};
    CanvasRenderer r(*doc);
    Rect cov{};
    (void)r.renderRegionScaledCached(all, 4096, cov);

    pl->tiles().fillRect(all, Rgba8{7, 7, 7, 255});
    r.invalidateAll();
    const PixelBuffer after = r.renderRegionScaledCached(all, 4096, cov);

    CanvasRenderer fresh(*doc);
    Rect cov2{};
    const PixelBuffer rebuilt = fresh.renderRegionScaledCached(all, 4096, cov2);
    PE_CHECK(sameBuffer(after, rebuilt));
}

PE_TEST(scaledcache_matches_a_box_average_of_the_full_resolution_composite) {
    // Exactness, not merely self-consistency: each output pixel must be the premultiplied
    // box average of the block it covers in the full-resolution composite. Without this the
    // incremental path could be internally consistent and still wrong.
    auto doc = tiledDoc(2, 2);
    const Rect all{0, 0, 2 * kTileSize, 2 * kTileSize};
    CanvasRenderer r(*doc);
    Rect cov{};
    const PixelBuffer scaled = r.renderRegionScaledCached(all, 1024, cov);
    PE_CHECK(cov == all);
    const int s = all.width / scaled.width();
    PE_CHECK(s > 1);

    CanvasRenderer fullRes(*doc);
    const PixelBuffer full = fullRes.renderRegion(all);
    PE_CHECK(!full.isEmpty());

    for (int oy = 0; oy < scaled.height(); ++oy) {
        for (int ox = 0; ox < scaled.width(); ++ox) {
            double sr = 0.0;
            double sg = 0.0;
            double sb = 0.0;
            double sa = 0.0;
            for (int y = 0; y < s; ++y) {
                for (int x = 0; x < s; ++x) {
                    const Rgba8 p = full.at(ox * s + x, oy * s + y);
                    const double a = p.a / 255.0;
                    sr += (p.r / 255.0) * a;
                    sg += (p.g / 255.0) * a;
                    sb += (p.b / 255.0) * a;
                    sa += a;
                }
            }
            const double n = static_cast<double>(s) * s;
            const Rgba8 got = scaled.at(ox, oy);
            PE_CHECK(std::abs(static_cast<int>(got.a) -
                              static_cast<int>(std::lround(sa / n * 255.0))) <= 1);
            if (sa > 0.0) {
                PE_CHECK(std::abs(static_cast<int>(got.r) -
                                  static_cast<int>(std::lround(sr / sa * 255.0))) <= 1);
                PE_CHECK(std::abs(static_cast<int>(got.g) -
                                  static_cast<int>(std::lround(sg / sa * 255.0))) <= 1);
            }
        }
    }
}

PE_TEST(renderregion_over_the_whole_canvas_equals_compositeimage) {
    // The Magic Wand samples the flattened canvas. It used to do that through
    // Document::compositeImage(), which recomposites every layer of every tile on every
    // click; going through the renderer instead is only legitimate if the pixels are
    // IDENTICAL, not merely similar. Several tiles across and several layers deep, so a
    // per-tile or per-layer discrepancy has somewhere to show up.
    auto doc = Document::createBlank(Size{3 * kTileSize, 2 * kTileSize});
    auto* base = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            base->tiles().fillRect(Rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize},
                                   Rgba8{static_cast<std::uint8_t>(20 + 60 * col),
                                         static_cast<std::uint8_t>(20 + 60 * row), 180, 255});
        }
    }
    auto overlay = std::make_unique<PixelLayer>();
    overlay->setOpacity(0.4f);  // a partial layer, so the blend has to agree too
    overlay->tiles().fillRect(Rect{kTileSize - 40, 30, 2 * kTileSize, kTileSize + 50},
                              Rgba8{250, 240, 10, 200});
    doc->cmdInsertTopLevel(1, std::move(overlay));

    const Rect all = doc->canvasBounds();
    const PixelBuffer flat = doc->compositeImage();
    CanvasRenderer r(*doc);
    const PixelBuffer viaCache = r.renderRegion(all);
    PE_CHECK_EQ(viaCache.width(), flat.width());
    PE_CHECK_EQ(viaCache.height(), flat.height());
    if (viaCache.width() != flat.width() || viaCache.height() != flat.height()) return;

    int differing = 0;
    for (int y = 0; y < flat.height(); ++y) {
        for (int x = 0; x < flat.width(); ++x) {
            if (!(flat.at(x, y) == viaCache.at(x, y))) ++differing;
        }
    }
    PE_CHECK_EQ(differing, 0);  // byte-identical, so the swap is not a behaviour change
}

PE_TEST(renderregion_over_the_whole_canvas_is_free_the_second_time) {
    // And the reason for the swap: a second wand click, or one after an edit, must pay for
    // the tiles that actually changed rather than for the whole canvas again.
    auto doc = Document::createBlank(Size{3 * kTileSize, 2 * kTileSize});
    auto* base = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    base->tiles().fillRect(Rect{0, 0, 3 * kTileSize, 2 * kTileSize}, kBlue);
    const Rect all = doc->canvasBounds();

    CanvasRenderer r(*doc);
    (void)r.renderRegion(all);
    const std::uint64_t afterFirst = r.recompositeCount();
    PE_CHECK_EQ(afterFirst, static_cast<std::uint64_t>(6));  // all six tiles, once

    (void)r.renderRegion(all);
    PE_CHECK_EQ(r.recompositeCount(), afterFirst);  // nothing changed, so nothing recomposited

    // One tile's worth of change costs one tile, not six.
    base->tiles().fillRect(Rect{kTileSize + 10, 10, 20, 20}, kRed);
    r.invalidate(Rect{kTileSize + 10, 10, 20, 20});
    (void)r.renderRegion(all);
    PE_CHECK_EQ(r.recompositeCount() - afterFirst, static_cast<std::uint64_t>(1));
}
