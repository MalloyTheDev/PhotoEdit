// Locality and containment for the region-bake brushes (Blur and Sharpen).
//
// Both convolve, and the convolution replicates at the edge of whatever rectangle it is
// handed. That rectangle used to be the stroke's coverage bounding box, which grows as the
// stroke continues, so a pixel within one kernel radius of that edge was convolved against
// replicated values and its result depended on where else the stroke went. Measured before
// the fix: a far excursion that never came near a spot moved pixels there by up to 110/255.
//
// The fix is to run the bake over the coverage box grown by the kernel reach, which puts
// the replicated edge a full kernel away from every pixel that is actually blended. These
// tests pin the two properties that gives, and they are properties rather than golden
// comparisons on purpose: they hold for any correct implementation, including the
// incremental one.

#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"
#include "pixeldiff.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pe;

namespace {

constexpr int kW = 160;
constexpr int kH = 120;
const Rect kCanvas{0, 0, kW, kH};

BrushSettings smallBrush() {
    BrushSettings b;
    b.diameter = 9.0f;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    return b;
}

// High-frequency content everywhere, so a blur or sharpen has an effect at every pixel and
// a contaminated one is visible rather than lost in flat colour.
std::unique_ptr<Document> noisyDoc(LayerId& out) {
    auto doc = Document::createBlank(Size{kW, kH});
    out = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(out));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            pl->tiles().setPixel(x, y,
                                 Rgba8{static_cast<std::uint8_t>((x * 37) % 256),
                                       static_cast<std::uint8_t>((y * 53) % 256),
                                       static_cast<std::uint8_t>(((x + y) * 29) % 256), 255});
        }
    }
    return doc;
}

const TileStoreT<Rgba8>& tilesOf(Document& doc, LayerId id) {
    return static_cast<PixelLayer*>(doc.findLayer(id))->tiles();
}

using StrokeFn = std::unique_ptr<PaintCommand> (*)(Document&, LayerId, const BrushSettings&,
                                                   std::span<const StrokePoint>, const Selection*);

// Run `stroke` on a fresh document and return it, so two runs can be compared.
std::unique_ptr<Document> runStroke(StrokeFn stroke, const std::vector<StrokePoint>& pts,
                                    LayerId& out) {
    auto doc = noisyDoc(out);
    auto cmd = stroke(*doc, out, smallBrush(), pts, nullptr);
    if (cmd != nullptr) cmd->execute(*doc);
    return doc;
}

// A short stroke near (30,30), and the same stroke continued far away. The continuation's
// own dabs start at (40,30) with a 4.5 px radius, so nothing at x <= 33 is inside its
// coverage and that column band must come out identical either way.
const std::vector<StrokePoint>& shortStroke() {
    static const std::vector<StrokePoint> pts{StrokePoint{Vec2{30.0f, 30.0f}, 1.0f},
                                              StrokePoint{Vec2{40.0f, 30.0f}, 1.0f}};
    return pts;
}
std::vector<StrokePoint> strokeWithFarExcursion() {
    std::vector<StrokePoint> pts = shortStroke();
    pts.push_back(StrokePoint{Vec2{140.0f, 100.0f}, 1.0f});
    return pts;
}

// `band` must be a region the continuation's own dabs never reach. Both runs place
// identical dabs there, so a correct filter produces identical pixels; a difference means
// the result depended on how far away the stroke went afterwards.
//
// Guarded against passing vacuously: the band has to be somewhere the SHORT stroke actually
// changed, or "no difference" would just mean "nothing happened here either way".
void checkLocality(StrokeFn stroke, const std::vector<StrokePoint>& shortPts,
                   const std::vector<StrokePoint>& longPts, Rect band) {
    LayerId shortId = kNoLayer;
    LayerId longId = kNoLayer;
    LayerId baseId = kNoLayer;
    auto shortDoc = runStroke(stroke, shortPts, shortId);
    auto longDoc = runStroke(stroke, longPts, longId);
    auto base = noisyDoc(baseId);

    const pe_diff::Diff touched =
        pe_diff::compare(tilesOf(*base, baseId), tilesOf(*shortDoc, shortId), band);
    PE_CHECK(touched.changed > 0);  // the band is somewhere the stroke really painted

    const pe_diff::Diff d =
        pe_diff::compare(tilesOf(*shortDoc, shortId), tilesOf(*longDoc, longId), band);
    PE_CHECK_EQ(d.changed, 0);
    PE_CHECK_EQ(d.worstChannel, 0);
}

// The dab footprint of a stroke: every sample inflated by the brush radius. Coverage cannot
// reach outside this, so neither can any pixel the brush is allowed to write.
Rect dabFootprint(const std::vector<StrokePoint>& pts, float diameter) {
    const auto r = static_cast<int>(diameter * 0.5f) + 2;  // +2 for the dab's own rounding
    Rect out{};
    for (const StrokePoint& p : pts) {
        const int x = static_cast<int>(p.pos.x);
        const int y = static_cast<int>(p.pos.y);
        out = out.united(Rect{x - r, y - r, 2 * r + 1, 2 * r + 1});
    }
    return out;
}

void checkContainment(StrokeFn stroke, const std::vector<StrokePoint>& pts) {
    LayerId beforeId = kNoLayer;
    LayerId afterId = kNoLayer;
    auto before = noisyDoc(beforeId);
    auto after = runStroke(stroke, pts, afterId);

    const pe_diff::Diff d =
        pe_diff::compare(tilesOf(*before, beforeId), tilesOf(*after, afterId), kCanvas);
    // The permitted influence box. The kernel reach widens what the filter READS, never
    // what it writes: the blend is gated by coverage, so only covered pixels move.
    const Rect permitted = dabFootprint(pts, smallBrush().diameter);
    PE_CHECK(pe_diff::containedIn(d.bounds, permitted));
}

}  // namespace

PE_TEST(blurbrush_result_does_not_depend_on_where_else_the_stroke_went) {
    checkLocality(&blurStroke, shortStroke(), strokeWithFarExcursion(), Rect{0, 0, 34, kH});
}

PE_TEST(sharpenbrush_result_does_not_depend_on_where_else_the_stroke_went) {
    checkLocality(&sharpenStroke, shortStroke(), strokeWithFarExcursion(), Rect{0, 0, 34, kH});
}

PE_TEST(blurbrush_changes_nothing_outside_its_dab_footprint) {
    // Spatial containment. An implementation can be numerically plausible and still touch
    // pixels it had no business touching; the kernel reach must widen reads only.
    checkContainment(&blurStroke, shortStroke());
    checkContainment(&blurStroke, strokeWithFarExcursion());
}

PE_TEST(sharpenbrush_changes_nothing_outside_its_dab_footprint) {
    checkContainment(&sharpenStroke, shortStroke());
    checkContainment(&sharpenStroke, strokeWithFarExcursion());
}

PE_TEST(regionbrush_single_dab_is_local_and_contained) {
    // Pathological: one sample. The coverage box is then smaller than the kernel, which is
    // the case where growing it by the reach matters most.
    const std::vector<StrokePoint> one{StrokePoint{Vec2{80.0f, 60.0f}, 1.0f}};
    checkContainment(&blurStroke, one);
    checkContainment(&sharpenStroke, one);

    // Locality for the same case. The continuation runs straight along +x so the geometry
    // is one-dimensional: its first interpolated dab sits one spacing step (0.25 * 9 =
    // 2.25 px) along, and with a 4.5 px radius reaches back to x = 77.75. The single dab
    // itself spans x = 75.5 to 84.5, so columns 75 and 76 are painted by the dab and never
    // reached by the continuation. That two-column band is thin because the spacing makes
    // it so, not because the test is being loose; checkLocality asserts the dab really
    // changed it, so it cannot pass by measuring nothing.
    std::vector<StrokePoint> dabPlusFar = one;
    dabPlusFar.push_back(StrokePoint{Vec2{150.0f, 60.0f}, 1.0f});
    checkLocality(&blurStroke, one, dabPlusFar, Rect{0, 0, 77, kH});
    checkLocality(&sharpenStroke, one, dabPlusFar, Rect{0, 0, 77, kH});
}

PE_TEST(regionbrush_at_the_canvas_border_stays_inside_the_canvas) {
    // Pathological: a stroke whose dabs and whose kernel halo both run off the canvas. The
    // bake reads outside (absent tiles are transparent, which is what the layer genuinely
    // holds there), but it must not write outside the region it covers, and it must not
    // fail.
    const std::vector<StrokePoint> edge{StrokePoint{Vec2{1.0f, 1.0f}, 1.0f},
                                        StrokePoint{Vec2{1.0f, 30.0f}, 1.0f}};
    checkContainment(&blurStroke, edge);
    checkContainment(&sharpenStroke, edge);

    LayerId id = kNoLayer;
    auto doc = runStroke(&blurStroke, edge, id);
    LayerId baseId = kNoLayer;
    auto base = noisyDoc(baseId);
    const pe_diff::Diff d = pe_diff::compare(tilesOf(*base, baseId), tilesOf(*doc, id), kCanvas);
    PE_CHECK(d.changed > 0);  // it did something rather than silently refusing
}

PE_TEST(regionbrush_selection_gate_does_not_widen_the_footprint) {
    // A gated stroke may write less than an ungated one, never more.
    Selection sel;
    sel.selectRect(Rect{28, 28, 6, 6});
    LayerId gatedId = kNoLayer;
    auto gated = noisyDoc(gatedId);
    auto cmd = blurStroke(*gated, gatedId, smallBrush(), shortStroke(), &sel);
    PE_CHECK(cmd != nullptr);
    cmd->execute(*gated);

    LayerId baseId = kNoLayer;
    auto base = noisyDoc(baseId);
    const pe_diff::Diff d =
        pe_diff::compare(tilesOf(*base, baseId), tilesOf(*gated, gatedId), kCanvas);
    PE_CHECK(pe_diff::containedIn(d.bounds, Rect{28, 28, 6, 6}));
    PE_CHECK(d.changed > 0);
}

namespace {

using LiveFn = std::unique_ptr<LiveStroke> (*)(Document&, LayerId, const BrushSettings&,
                                               const Selection*);

// The incremental stroke, fed in `chunk`-sized steps, must leave the layer byte-identical
// to the batched one, and commit a command that undoes and redoes byte-exactly.
//
// Byte-identical rather than within a tolerance, and that is only possible because the
// batched path no longer replicates at the edge of a box that grows with the stroke. While
// it did, a pixel's value depended on how long the stroke had become and nothing tile-local
// could reproduce it.
void checkRegionParity(StrokeFn batch, LiveFn live, const std::vector<StrokePoint>& pts,
                       const Selection* sel, int chunk) {
    LayerId ba = kNoLayer;
    auto da = noisyDoc(ba);
    auto cmdA = batch(*da, ba, smallBrush(), pts, sel);
    PE_CHECK(cmdA != nullptr);
    cmdA->execute(*da);

    LayerId bb = kNoLayer;
    auto db = noisyDoc(bb);
    LayerId initId = kNoLayer;
    auto initial = noisyDoc(initId);
    auto stroke = live(*db, bb, smallBrush(), sel);
    PE_CHECK(stroke != nullptr);
    std::vector<StrokePoint> acc;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        acc.push_back(pts[i]);
        if (static_cast<int>(acc.size()) % chunk == 0 || i + 1 == pts.size()) {
            (void)stroke->extend(acc);
        }
    }
    auto cmdB = stroke->finish();
    PE_CHECK(cmdB != nullptr);

    const pe_diff::Diff d = pe_diff::compare(tilesOf(*da, ba), tilesOf(*db, bb), kCanvas);
    PE_CHECK_EQ(d.changed, 0);
    PE_CHECK_EQ(d.worstChannel, 0);

    // One byte-exact undo step: the store already holds the final pixels, so pushing
    // re-applies a no-op, undo restores the original and redo reproduces the result.
    db->history().push(std::move(cmdB));
    PE_CHECK_EQ(pe_diff::compare(tilesOf(*da, ba), tilesOf(*db, bb), kCanvas).changed, 0);
    db->history().undo();
    PE_CHECK_EQ(pe_diff::compare(tilesOf(*initial, initId), tilesOf(*db, bb), kCanvas).changed, 0);
    db->history().redo();
    PE_CHECK_EQ(pe_diff::compare(tilesOf(*da, ba), tilesOf(*db, bb), kCanvas).changed, 0);
}

std::vector<StrokePoint> longDiagonal() {
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 18; ++i) {
        const auto t = static_cast<float>(i);
        pts.push_back(StrokePoint{Vec2{18.0f + t * 7.0f, 18.0f + t * 5.0f}, 0.5f + 0.02f * t});
    }
    return pts;
}

}  // namespace

PE_TEST(liveblur_matches_batched_one_sample_at_a_time) {
    checkRegionParity(&blurStroke, &beginBlurStroke, longDiagonal(), nullptr, 1);
}

PE_TEST(livesharpen_matches_batched_one_sample_at_a_time) {
    checkRegionParity(&sharpenStroke, &beginSharpenStroke, longDiagonal(), nullptr, 1);
}

PE_TEST(liveregion_matches_batched_in_uneven_chunks) {
    // The halo has to be drawn from pre-stroke pixels even where a NEIGHBOURING tile has
    // already been rewritten by an earlier sample, which is what chunking exposes.
    for (const int chunk : {2, 3, 5}) {
        checkRegionParity(&blurStroke, &beginBlurStroke, longDiagonal(), nullptr, chunk);
        checkRegionParity(&sharpenStroke, &beginSharpenStroke, longDiagonal(), nullptr, chunk);
    }
}

PE_TEST(liveregion_matches_batched_when_gated_by_a_selection) {
    Selection sel;
    sel.selectRect(Rect{30, 0, 60, kH});
    checkRegionParity(&blurStroke, &beginBlurStroke, longDiagonal(), &sel, 1);
    checkRegionParity(&sharpenStroke, &beginSharpenStroke, longDiagonal(), &sel, 4);
}

PE_TEST(liveregion_matches_batched_for_a_single_dab_and_at_the_canvas_edge) {
    // The two pathological shapes: a coverage box smaller than the kernel, and one whose
    // halo runs off the canvas.
    const std::vector<StrokePoint> one{StrokePoint{Vec2{80.0f, 60.0f}, 1.0f}};
    checkRegionParity(&blurStroke, &beginBlurStroke, one, nullptr, 1);
    const std::vector<StrokePoint> edge{StrokePoint{Vec2{1.0f, 1.0f}, 1.0f},
                                        StrokePoint{Vec2{1.0f, 30.0f}, 1.0f}};
    checkRegionParity(&blurStroke, &beginBlurStroke, edge, nullptr, 1);
    checkRegionParity(&sharpenStroke, &beginSharpenStroke, edge, nullptr, 2);
}

PE_TEST(liveregion_per_sample_dirty_stays_bounded) {
    // The acceptance criterion: per-sample work must not grow with stroke length. The
    // dirty rect is the proxy, bounded by the tiles the newest dabs reach.
    auto doc = Document::createBlank(Size{2048, 2048});
    auto id = doc->activeLayer();
    auto& tiles = static_cast<PixelLayer*>(doc->findLayer(id))->tiles();
    // Striped, not flat: blurring a constant colour changes nothing, so a flat canvas
    // would report an empty dirty rect every sample and the test would measure nothing.
    tiles.fillRect(Rect{0, 0, 2048, 2048}, Rgba8{90, 110, 130, 255});
    for (int x = 0; x < 2048; x += 64) {
        tiles.fillRect(Rect{x, 0, 32, 2048}, Rgba8{220, 40, 60, 255});
    }
    auto stroke = beginBlurStroke(*doc, id, smallBrush(), nullptr);
    PE_CHECK(stroke != nullptr);

    std::vector<StrokePoint> acc;
    int widest = 0;
    Rect cumulative{};
    for (int i = 0; i <= 80; ++i) {
        const float d = 20.0f + static_cast<float>(i) * 24.0f;
        acc.push_back(StrokePoint{Vec2{d, d}, 1.0f});
        const Rect r = stroke->extend(acc);
        widest = std::max(widest, r.width);
        cumulative = cumulative.united(r);
    }
    PE_CHECK(cumulative.width >= 1024);  // the stroke really crossed the canvas
    PE_CHECK(widest <= 2 * kTileSize);   // a constant, independent of stroke length
    PE_CHECK(widest < cumulative.width);
    PE_CHECK(stroke->finish() != nullptr);
}

namespace {

// A canvas several tiles across, so a stroke crosses tile boundaries. That is the case the
// single-tile fixture above cannot reach, and it is where the two things most likely to go
// wrong actually bite: a tile's halo reaching into a NEIGHBOUR the stroke has already
// rewritten (which must still read pre-stroke pixels), and the convolution cache returning
// a result computed for a different tile.
constexpr int kBigW = 3 * kTileSize;
constexpr int kBigH = 2 * kTileSize;
const Rect kBigCanvas{0, 0, kBigW, kBigH};

std::unique_ptr<Document> bigNoisyDoc(LayerId& out) {
    auto doc = Document::createBlank(Size{kBigW, kBigH});
    out = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(out));
    for (int y = 0; y < kBigH; ++y) {
        for (int x = 0; x < kBigW; ++x) {
            pl->tiles().setPixel(x, y,
                                 Rgba8{static_cast<std::uint8_t>((x * 37) % 256),
                                       static_cast<std::uint8_t>((y * 53) % 256),
                                       static_cast<std::uint8_t>(((x + y) * 29) % 256), 255});
        }
    }
    return doc;
}

// A stroke that walks across two tile seams and back over one of them, so at least one tile
// is re-derived after its neighbour has already been written.
std::vector<StrokePoint> acrossTileSeams() {
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 30; ++i) {
        const auto t = static_cast<float>(i);
        pts.push_back(StrokePoint{
            Vec2{kTileSize - 40.0f + t * 16.0f, 120.0f + 40.0f * std::sin(t * 0.4f)}, 1.0f});
    }
    for (int i = 0; i <= 10; ++i) {  // double back over the first seam
        const auto t = static_cast<float>(i);
        pts.push_back(StrokePoint{Vec2{kTileSize + 100.0f - t * 16.0f, 150.0f}, 1.0f});
    }
    return pts;
}

void checkMultiTileParity(StrokeFn batch, LiveFn live, const Selection* sel, int chunk) {
    const std::vector<StrokePoint> pts = acrossTileSeams();
    LayerId ba = kNoLayer;
    auto da = bigNoisyDoc(ba);
    auto cmdA = batch(*da, ba, smallBrush(), pts, sel);
    PE_CHECK(cmdA != nullptr);
    cmdA->execute(*da);

    LayerId bb = kNoLayer;
    auto db = bigNoisyDoc(bb);
    auto stroke = live(*db, bb, smallBrush(), sel);
    PE_CHECK(stroke != nullptr);
    std::vector<StrokePoint> acc;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        acc.push_back(pts[i]);
        if (static_cast<int>(acc.size()) % chunk == 0 || i + 1 == pts.size()) {
            (void)stroke->extend(acc);
        }
    }
    PE_CHECK(stroke->finish() != nullptr);

    const pe_diff::Diff d = pe_diff::compare(tilesOf(*da, ba), tilesOf(*db, bb), kBigCanvas);
    PE_CHECK_EQ(d.changed, 0);
    PE_CHECK_EQ(d.worstChannel, 0);
    // Not vacuous: the stroke has to have crossed more than one tile.
    PE_CHECK(d.bounds.isEmpty());
    LayerId baseId = kNoLayer;
    auto base = bigNoisyDoc(baseId);
    const pe_diff::Diff painted =
        pe_diff::compare(tilesOf(*base, baseId), tilesOf(*db, bb), kBigCanvas);
    PE_CHECK(painted.bounds.width > kTileSize);
}

}  // namespace

PE_TEST(liveblur_matches_batched_across_tile_seams) {
    checkMultiTileParity(&blurStroke, &beginBlurStroke, nullptr, 1);
    checkMultiTileParity(&blurStroke, &beginBlurStroke, nullptr, 3);
}

PE_TEST(livesharpen_matches_batched_across_tile_seams) {
    checkMultiTileParity(&sharpenStroke, &beginSharpenStroke, nullptr, 1);
    checkMultiTileParity(&sharpenStroke, &beginSharpenStroke, nullptr, 4);
}

PE_TEST(liveregion_matches_batched_across_tile_seams_when_gated) {
    // Wide enough to span a tile seam: a selection narrower than a tile would confine the
    // painted region to one tile and the non-vacuity guard below would (correctly) refuse.
    Selection sel;
    sel.selectRect(Rect{kTileSize - 100, 0, 400, kBigH});
    checkMultiTileParity(&blurStroke, &beginBlurStroke, &sel, 2);
}
