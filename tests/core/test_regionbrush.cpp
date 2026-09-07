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
