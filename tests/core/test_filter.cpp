#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace pe;

namespace {
std::vector<Rgbaf> grayRow(std::vector<float> vals) {
    std::vector<Rgbaf> img;
    for (float v : vals) img.push_back(Rgbaf{v, v, v, 1.0f});
    return img;
}

int alphaAt(Document& doc, LayerId id, int x, int y) {
    return static_cast<PixelLayer*>(doc.findLayer(id))->tiles().pixel(x, y).a;
}
int redAt(Document& doc, LayerId id, int x, int y) {
    return static_cast<PixelLayer*>(doc.findLayer(id))->tiles().pixel(x, y).r;
}
}  // namespace

PE_TEST(filter_gaussian_zero_sigma_is_identity) {
    auto src = grayRow({0.1f, 0.5f, 0.9f, 0.2f});
    std::vector<Rgbaf> dst(src.size());
    gaussianBlur(src, dst, 4, 1, 0.0f);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
}

PE_TEST(filter_gaussian_nonfinite_or_huge_sigma_is_safe) {
    // NaN/Inf sigma must never reach the (int)ceil(3*sigma) conversion (out-of-range float->int =
    // UB; the old `sigma <= 0` test let them through). A huge finite sigma must not overflow
    // 2*radius+1 or run unbounded — the radius is capped inside the kernel.
    auto src = grayRow({0.0f, 1.0f, 0.0f, 1.0f, 0.0f});
    std::vector<Rgbaf> dst(src.size());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    gaussianBlur(src, dst, 5, 1, nan);  // -> identity copy
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
    gaussianBlur(src, dst, 5, 1, inf);  // -> identity copy
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
    gaussianBlur(src, dst, 5, 1, 1e9f);  // huge finite: bounded radius, terminates, stays finite
    for (const Rgbaf& p : dst) PE_CHECK(std::isfinite(p.r));
    unsharpMask(src, dst, 5, 1, inf, 1.0f, 0.0f);  // radius forwarded as sigma -> safe
    for (const Rgbaf& p : dst) PE_CHECK(std::isfinite(p.r));
}

PE_TEST(filter_box_radius_zero_is_identity) {
    auto src = grayRow({0.1f, 0.5f, 0.9f, 0.2f});
    std::vector<Rgbaf> dst(src.size());
    boxBlur(src, dst, 4, 1, 0);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
}

PE_TEST(filter_blur_preserves_constant) {
    std::vector<Rgbaf> src(25, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});  // 5x5 uniform
    std::vector<Rgbaf> dst(25);
    gaussianBlur(src, dst, 5, 5, 1.0f);
    PE_CHECK_NEAR(dst[12].r, 0.5f);  // center unchanged (clamped border preserves it)
    PE_CHECK_NEAR(dst[0].r, 0.5f);   // corner too
}

PE_TEST(filter_blur_softens_step_edge) {
    auto src = grayRow({1, 1, 1, 1, 0, 0, 0});  // white | black
    std::vector<Rgbaf> dst(src.size());
    gaussianBlur(src, dst, 7, 1, 1.0f);
    PE_CHECK(dst[3].r < 0.99f);  // last white pixel darkened by the black neighbor
    PE_CHECK(dst[4].r > 0.01f);  // first black pixel lightened
}

PE_TEST(filter_unsharp_amount_zero_is_identity) {
    auto src = grayRow({0.2f, 0.5f, 0.8f, 0.3f});
    std::vector<Rgbaf> dst(src.size());
    unsharpMask(src, dst, 4, 1, 1.0f, 0.0f, 0.0f);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
}

PE_TEST(filter_unsharp_creates_halo) {
    auto src = grayRow({0.5f, 0.5f, 1.0f, 0.5f, 0.5f});  // a bright spike
    std::vector<Rgbaf> dst(src.size());
    unsharpMask(src, dst, 5, 1, 1.0f, 1.0f, 0.0f);
    PE_CHECK(dst[1].r < 0.5f);  // neighbor of the spike is darkened (sharpening halo)
}

PE_TEST(filter_blur_no_color_bleed_from_transparent) {
    // An opaque red next to a fully transparent pixel must not pick up the
    // transparent pixel's (arbitrary) color — premultiplied blur weights color by
    // coverage, so transparent pixels contribute zero color.
    std::vector<Rgbaf> src = {Rgbaf{1, 0, 0, 1}, Rgbaf{0, 0, 1, 0}};  // red opaque | blue transp.
    std::vector<Rgbaf> dst(2);
    boxBlur(src, dst, 2, 1, 1);
    PE_CHECK(dst[0].b < 0.01f);     // no blue bleed into the red pixel
    PE_CHECK_NEAR(dst[0].r, 1.0f);  // red color preserved
}

PE_TEST(filter_apply_command_and_undo) {
    auto doc = Document::createBlank(Size{32, 32});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 16, 32}, Rgba8{255, 255, 255, 255});  // white left half
    pl->tiles().fillRect(Rect{16, 0, 16, 32}, Rgba8{0, 0, 0, 255});       // black right half

    auto cmd = applyFilter(*doc, base, GaussianBlurFilter(2.0f));
    PE_CHECK(cmd != nullptr);
    PE_CHECK(cmd->touchedTileCount() <= static_cast<std::size_t>(1));  // 32x32 == one tile
    doc->history().push(std::move(cmd));

    PE_CHECK(redAt(*doc, base, 15, 16) < 255);  // edge blurred
    PE_CHECK(redAt(*doc, base, 16, 16) > 0);
    PE_CHECK_EQ(alphaAt(*doc, base, 15, 16), 255);  // alpha preserved (opaque)

    doc->history().undo();
    PE_CHECK_EQ(redAt(*doc, base, 15, 16), 255);  // restored white
    PE_CHECK_EQ(redAt(*doc, base, 16, 16), 0);    // restored black
}

PE_TEST(filter_on_empty_layer_is_null) {
    auto doc = Document::createBlank(Size{32, 32});
    PE_CHECK(applyFilter(*doc, doc->activeLayer(), GaussianBlurFilter(2.0f)) == nullptr);
}

PE_TEST(filter_mosaic_cell_one_is_identity) {
    auto src = grayRow({0.1f, 0.5f, 0.9f, 0.2f});
    std::vector<Rgbaf> dst(src.size());
    mosaic(src, dst, 4, 1, 1);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
}

PE_TEST(filter_mosaic_averages_block) {
    // A 4x1 row with one block of size 4 becomes the average (0.1+0.3+0.5+0.7)/4 = 0.4.
    auto src = grayRow({0.1f, 0.3f, 0.5f, 0.7f});
    std::vector<Rgbaf> dst(src.size());
    mosaic(src, dst, 4, 1, 4);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, 0.4f);
}

PE_TEST(filter_mosaic_no_color_bleed_from_transparent) {
    // Averaging in premultiplied alpha: a transparent pixel contributes no color.
    std::vector<Rgbaf> src = {Rgbaf{1, 0, 0, 1}, Rgbaf{0, 0, 1, 0}};  // red opaque | blue transp.
    std::vector<Rgbaf> dst(2);
    mosaic(src, dst, 2, 1, 2);
    PE_CHECK(dst[0].b < 0.01f);     // no blue
    PE_CHECK_NEAR(dst[0].r, 1.0f);  // color is the (coverage-weighted) red
    PE_CHECK_NEAR(dst[0].a, 0.5f);  // alpha is the straight average of 1 and 0
}

PE_TEST(filter_median_radius_zero_is_identity) {
    auto src = grayRow({0.1f, 0.5f, 0.9f, 0.2f});
    std::vector<Rgbaf> dst(src.size());
    medianFilter(src, dst, 4, 1, 0);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
}

PE_TEST(filter_median_removes_salt_speckle) {
    // A single bright speckle in a dark 3x3 field is erased by a radius-1 median.
    std::vector<Rgbaf> src(9, Rgbaf{0.2f, 0.2f, 0.2f, 1.0f});
    src[4] = Rgbaf{1.0f, 1.0f, 1.0f, 1.0f};  // center speckle
    std::vector<Rgbaf> dst(9);
    medianFilter(src, dst, 3, 3, 1);
    PE_CHECK_NEAR(dst[4].r, 0.2f);  // speckle replaced by the field's median
}

PE_TEST(filter_find_edges_flat_is_white) {
    std::vector<Rgbaf> src(25, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});  // 5x5 uniform
    std::vector<Rgbaf> dst(25);
    findEdges(src, dst, 5, 5);
    PE_CHECK_NEAR(dst[12].r, 1.0f);  // no gradient -> white
    PE_CHECK_NEAR(dst[0].r, 1.0f);   // clamped border has no gradient either
}

PE_TEST(filter_find_edges_marks_boundary) {
    // A vertical white|black step produces a dark edge at the boundary.
    auto src = grayRow({1, 1, 1, 0, 0, 0});
    std::vector<Rgbaf> dst(src.size());
    findEdges(src, dst, 6, 1);
    PE_CHECK(dst[2].r < 0.5f);      // last white pixel sits on the edge -> dark
    PE_CHECK_NEAR(dst[0].r, 1.0f);  // flat interior stays white
}

PE_TEST(filter_add_noise_amount_zero_is_identity) {
    auto src = grayRow({0.2f, 0.5f, 0.8f, 0.3f});
    std::vector<Rgbaf> dst(src.size());
    addNoise(src, dst, 4, 1, 0.0f, false, true, 1u);
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
}

PE_TEST(filter_add_noise_is_deterministic) {
    std::vector<Rgbaf> src(64, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});  // 8x8 gray
    std::vector<Rgbaf> a(64), b(64);
    addNoise(src, a, 8, 8, 0.3f, false, true, 42u);
    addNoise(src, b, 8, 8, 0.3f, false, true, 42u);
    for (std::size_t i = 0; i < 64; ++i) {
        PE_CHECK_NEAR(a[i].r, b[i].r);
        PE_CHECK_NEAR(a[i].g, b[i].g);
    }
    // A different seed gives a different field.
    std::vector<Rgbaf> c(64);
    addNoise(src, c, 8, 8, 0.3f, false, true, 7u);
    bool anyDiff = false;
    for (std::size_t i = 0; i < 64; ++i)
        if (std::fabs(a[i].r - c[i].r) > 1e-4f) anyDiff = true;
    PE_CHECK(anyDiff);
}

PE_TEST(filter_add_noise_stays_in_range_and_keeps_alpha) {
    std::vector<Rgbaf> src(64, Rgbaf{0.5f, 0.5f, 0.5f, 0.7f});
    std::vector<Rgbaf> dst(64);
    addNoise(src, dst, 8, 8, 1.0f, false, true, 3u);  // strong noise
    for (const Rgbaf& p : dst) {
        PE_CHECK(p.r >= 0.0f && p.r <= 1.0f);
        PE_CHECK(p.g >= 0.0f && p.g <= 1.0f);
        PE_CHECK(p.b >= 0.0f && p.b <= 1.0f);
        PE_CHECK_NEAR(p.a, 0.7f);  // alpha preserved
    }
}

PE_TEST(filter_add_noise_monochromatic_keeps_gray) {
    std::vector<Rgbaf> src(16, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});  // 4x4 gray
    std::vector<Rgbaf> dst(16);
    addNoise(src, dst, 4, 4, 0.2f, true, true, 5u);  // monochromatic
    for (const Rgbaf& p : dst) {
        PE_CHECK_NEAR(p.r, p.g);  // same noise added to each channel -> stays neutral
        PE_CHECK_NEAR(p.g, p.b);
    }
}

PE_TEST(filter_add_noise_color_channels_differ) {
    std::vector<Rgbaf> src(16, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});
    std::vector<Rgbaf> dst(16);
    addNoise(src, dst, 4, 4, 0.3f, false, true, 9u);  // independent per channel
    bool anyChannelDiff = false;
    for (const Rgbaf& p : dst)
        if (std::fabs(p.r - p.g) > 1e-4f) anyChannelDiff = true;
    PE_CHECK(anyChannelDiff);
}

PE_TEST(filter_add_noise_preserves_mean) {
    // Zero-mean Gaussian noise with light amount (little clamping) leaves the
    // average near the original gray level.
    std::vector<Rgbaf> src(2500, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});  // 50x50
    std::vector<Rgbaf> dst(2500);
    addNoise(src, dst, 50, 50, 0.1f, false, true, 123u);
    double sum = 0.0;
    for (const Rgbaf& p : dst) sum += p.r;
    const double mean = sum / 2500.0;
    PE_CHECK(mean > 0.48 && mean < 0.52);
}

PE_TEST(filter_on_16bit_layer_edits_native_store) {
    auto doc = Document::createBlank(Size{32, 32}, ColorMode::RGB, BitDepth::U16);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles16().fillRect(Rect{0, 0, 16, 32}, Rgba16{65535, 65535, 65535, 65535});  // white left
    pl->tiles16().fillRect(Rect{16, 0, 16, 32}, Rgba16{0, 0, 0, 65535});             // black right

    auto cmd = applyFilter(*doc, base, GaussianBlurFilter(2.0f));
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK(pl->tiles16().pixel(15, 16).r < 65535);  // edge blurred in the 16-bit store
    PE_CHECK(pl->tiles16().pixel(16, 16).r > 0);
    PE_CHECK(pl->tiles().empty());  // 8-bit store untouched

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles16().pixel(15, 16).r, static_cast<uint16_t>(65535));  // restored white
}

PE_TEST(brush_paints_16bit_layer_native_with_undo) {
    // A brush stroke on a 16-bit layer deposits into the 16-bit store at full
    // precision, leaves the 8-bit store untouched, and undoes exactly.
    auto doc = Document::createBlank(Size{16, 16}, ColorMode::RGB, BitDepth::U16);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    std::vector<StrokePoint> pts{StrokePoint{Vec2{8.0f, 8.0f}, 1.0f}};
    BrushSettings s;
    s.diameter = 6.0f;
    s.hardness = 1.0f;

    auto cmd = paintStroke(*doc, base, s, Rgbaf{1.0f, 0.0f, 0.0f, 1.0f}, pts);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    const Rgba16 center = pl->tiles16().pixel(8, 8);
    PE_CHECK_EQ(center.r, static_cast<uint16_t>(65535));  // opaque red at full 16-bit
    PE_CHECK_EQ(center.a, static_cast<uint16_t>(65535));
    PE_CHECK(pl->tiles().empty());  // 8-bit store never touched

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles16().pixel(8, 8), (Rgba16{0, 0, 0, 0}));  // back to transparent
}

PE_TEST(move_layer_content_shifts_and_undoes) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().setPixel(10, 10, Rgba8{200, 50, 50, 255});  // a distinct mark

    auto cmd = moveLayerContent(*doc, base, 5, 3);  // shift +5,+3
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(15, 13), (Rgba8{200, 50, 50, 255}));  // moved here
    PE_CHECK_EQ(alphaAt(*doc, base, 10, 10), 0);                        // vacated -> transparent

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{200, 50, 50, 255}));  // restored to origin
    PE_CHECK_EQ(alphaAt(*doc, base, 15, 13), 0);
}

PE_TEST(move_layer_content_edge_cases) {
    auto doc = Document::createBlank(Size{32, 32});
    const LayerId base = doc->activeLayer();
    PE_CHECK(moveLayerContent(*doc, base, 4, 4) == nullptr);  // empty layer: nothing to move

    static_cast<PixelLayer*>(doc->findLayer(base))->tiles().setPixel(5, 5, Rgba8{1, 2, 3, 255});
    PE_CHECK(moveLayerContent(*doc, base, 0, 0) == nullptr);       // zero move: no command
    PE_CHECK(moveLayerContent(*doc, base, 999999, 0) == nullptr);  // offset beyond the size cap
}

namespace {

// What a Move means, stated independently of how it is implemented: every pixel comes from
// (x-dx, y-dy) if that lies in the pre-move content bounds, and is transparent otherwise.
// The optimized path resolves a source tile once per run and clears whole spans at a time;
// this reads one pixel at a time through the store's own accessor and so cannot share a bug
// with it. Same role the per-pixel oracle played for the .pedoc gather in #176.
void checkMoveMatchesReference(Document& doc, LayerId id, int dx, int dy, Rect probe) {
    auto* pl = static_cast<PixelLayer*>(doc.findLayer(id));
    const Rect src = pl->contentBounds();
    std::vector<Rgba8> want;
    want.reserve(static_cast<std::size_t>(probe.width) * static_cast<std::size_t>(probe.height));
    for (int y = probe.top(); y < probe.bottom(); ++y) {
        for (int x = probe.left(); x < probe.right(); ++x) {
            const int sx = x - dx;
            const int sy = y - dy;
            want.push_back(src.contains(Point{sx, sy}) ? pl->tiles().pixel(sx, sy) : Rgba8{});
        }
    }
    auto cmd = moveLayerContent(doc, id, dx, dy);
    PE_REQUIRE(cmd != nullptr);
    doc.history().push(std::move(cmd));

    std::size_t i = 0;
    int mismatches = 0;
    for (int y = probe.top(); y < probe.bottom(); ++y) {
        for (int x = probe.left(); x < probe.right(); ++x, ++i) {
            if (!(pl->tiles().pixel(x, y) == want[i])) ++mismatches;
        }
    }
    PE_CHECK_EQ(mismatches, 0);
}

}  // namespace

PE_TEST(move_matches_a_per_pixel_reference_across_tile_boundaries) {
    // The shift is built one destination tile at a time, resolving the source tile once per
    // run, so every interesting case is a run that starts or ends on a tile edge. Offsets
    // cover: inside one tile, exactly one tile, one past a tile, negative on each axis, and
    // a diagonal that crosses in both at once.
    constexpr int T = kTileSize;
    for (const Point d : {Point{5, 3}, Point{-5, -3}, Point{T, 0}, Point{0, T}, Point{T + 1, T - 1},
                          Point{-T, T}, Point{1, -1}, Point{-(T + 7), T + 7}}) {
        auto doc = Document::createBlank(Size{3 * T, 2 * T});
        PE_REQUIRE(doc != nullptr);
        const LayerId base = doc->activeLayer();
        auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
        // Content that spans several tiles, has gaps, and lands on the tile grid: a block
        // straddling the first boundary, lone pixels at exact edges, and a far corner.
        pl->tiles().fillRect(Rect{T - 4, T - 4, 9, 9}, Rgba8{200, 50, 50, 255});
        pl->tiles().setPixel(T, T, Rgba8{9, 9, 9, 255});
        pl->tiles().setPixel(T - 1, 0, Rgba8{7, 7, 7, 255});
        pl->tiles().setPixel(2 * T + 5, T + 5, Rgba8{3, 4, 5, 255});
        checkMoveMatchesReference(*doc, base, d.x, d.y, Rect{-T, -T, 5 * T, 4 * T});
    }
}

PE_TEST(move_round_trips_exactly_at_every_depth) {
    constexpr int T = kTileSize;
    for (const BitDepth depth : {BitDepth::U8, BitDepth::U16, BitDepth::F32}) {
        auto doc = Document::createBlank(Size{2 * T, T}, ColorMode::RGB, depth, 96);
        PE_REQUIRE(doc != nullptr);
        const LayerId base = doc->activeLayer();
        auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
        switch (depth) {
            case BitDepth::U16:
                pl->tiles16().fillRect(Rect{T - 3, 4, 7, 7}, Rgba16{1000, 2000, 3000, 65535});
                break;
            case BitDepth::F32:
                pl->tilesF().fillRect(Rect{T - 3, 4, 7, 7}, Rgbaf{0.25f, 0.5f, 0.75f, 1.0f});
                break;
            case BitDepth::U8:
            default:
                pl->tiles().fillRect(Rect{T - 3, 4, 7, 7}, Rgba8{10, 20, 30, 255});
                break;
        }
        auto cmd = moveLayerContent(*doc, base, T + 2, 5);
        PE_REQUIRE(cmd != nullptr);
        doc->history().push(std::move(cmd));
        switch (depth) {
            case BitDepth::U16:
                PE_CHECK(pl->tiles16().pixel(2 * T - 1, 9) == (Rgba16{1000, 2000, 3000, 65535}));
                PE_CHECK(pl->tiles16().pixel(T - 3, 4) == (Rgba16{0, 0, 0, 0}));
                break;
            case BitDepth::F32: {
                const Rgbaf p = pl->tilesF().pixel(2 * T - 1, 9);
                PE_CHECK(p.r == 0.25f && p.g == 0.5f && p.b == 0.75f && p.a == 1.0f);
                PE_CHECK(pl->tilesF().pixel(T - 3, 4).a == 0.0f);
                break;
            }
            case BitDepth::U8:
            default:
                PE_CHECK(pl->tiles().pixel(2 * T - 1, 9) == (Rgba8{10, 20, 30, 255}));
                PE_CHECK(pl->tiles().pixel(T - 3, 4) == (Rgba8{0, 0, 0, 0}));
                break;
        }
        doc->history().undo();
        switch (depth) {
            case BitDepth::U16:
                PE_CHECK(pl->tiles16().pixel(T - 3, 4) == (Rgba16{1000, 2000, 3000, 65535}));
                break;
            case BitDepth::F32:
                PE_CHECK(pl->tilesF().pixel(T - 3, 4).a == 1.0f);
                break;
            case BitDepth::U8:
            default:
                PE_CHECK(pl->tiles().pixel(T - 3, 4) == (Rgba8{10, 20, 30, 255}));
                break;
        }
    }
}

PE_TEST(move_works_on_a_document_the_old_area_cap_refused) {
    // #180. A Move used to run through the generic float bake, whose kMaxFilterPixels limit
    // exists to bound several full-region float buffers. contentBounds() is tile-aligned, so
    // a 4000x4000 canvas reported 16.78 MP, over the 16 MP cap, and the Move tool silently
    // did nothing on any photograph from a modern camera. A translation allocates one tile
    // at a time and needs no float image, so that limit never applied to it.
    for (const Size canvas : {Size{4000, 4000}, Size{6000, 4000}}) {
        auto doc = Document::createBlank(canvas);
        PE_REQUIRE(doc != nullptr);
        const LayerId base = doc->activeLayer();
        auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
        pl->tiles().setPixel(10, 10, Rgba8{1, 2, 3, 255});
        pl->tiles().setPixel(canvas.width - 10, canvas.height - 10, Rgba8{4, 5, 6, 255});
        // Over the old cap, and by construction: tile-aligned bounds exceed 16 MP.
        const Rect bounds = pl->contentBounds();
        PE_CHECK(static_cast<std::int64_t>(bounds.width) * bounds.height > kMaxFilterPixels);

        auto cmd = moveLayerContent(*doc, base, 25, 25);
        PE_REQUIRE(cmd != nullptr);  // used to be nullptr, with no message to the user
        doc->history().push(std::move(cmd));
        PE_CHECK_EQ(pl->tiles().pixel(35, 35), (Rgba8{1, 2, 3, 255}));
        PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{0, 0, 0, 0}));
        doc->history().undo();
        PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{1, 2, 3, 255}));
    }
}

PE_TEST(move_leaves_no_empty_tiles_behind_so_the_layer_stays_filterable) {
    // A vacated tile used to be written back as a fully transparent tile rather than
    // removed, so contentBounds() became the source united with the destination and stayed
    // there. Every destructive filter is bounded by that rect, so one Move could leave a
    // layer permanently unfilterable over pixels that are all transparent.
    constexpr int T = kTileSize;
    auto doc = Document::createBlank(Size{8 * T, 8 * T});
    PE_REQUIRE(doc != nullptr);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, T, T}, Rgba8{10, 20, 30, 255});
    PE_CHECK(pl->contentBounds() == (Rect{0, 0, T, T}));

    auto cmd = moveLayerContent(*doc, base, 4 * T, 4 * T);
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));

    // Only the destination remains. Before the fix this was {0,0,5T,5T}, twenty-five times
    // the area, of which twenty-four twenty-fifths was transparent.
    PE_CHECK(pl->contentBounds() == (Rect{4 * T, 4 * T, T, T}));
    PE_CHECK_EQ(pl->tiles().tileCount(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(pl->tiles().pixel(4 * T + 5, 4 * T + 5), (Rgba8{10, 20, 30, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(5, 5), (Rgba8{0, 0, 0, 0}));

    // And undo restores the tile that was removed, not merely a transparent stand-in.
    doc->history().undo();
    PE_CHECK(pl->contentBounds() == (Rect{0, 0, T, T}));
    PE_CHECK_EQ(pl->tiles().tileCount(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(pl->tiles().pixel(5, 5), (Rgba8{10, 20, 30, 255}));
}

PE_TEST(move_costs_what_it_touches_not_the_span_between_source_and_destination) {
    // The budget and the work both used to be driven by the bounding box of source and
    // destination, which for a long drag is almost entirely empty space. A small layer
    // dragged a long way was refused for its DISTANCE rather than its content, and when
    // accepted it allocated and swept every tile in between.
    constexpr int T = kTileSize;
    auto doc = Document::createBlank(Size{4 * T, 4 * T});
    PE_REQUIRE(doc != nullptr);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{1, 2, 3, 255});  // one tile of content

    // A drag whose bounding box is 200x1 tiles. Only the source and destination tiles can
    // change, and only those two may be BUILT: sweeping the 198 empty tiles between them is
    // the defect. touchedTileCount reports tiles that changed, which is two either way, so
    // the cost needs its own counter to be observable at all.
    const std::uint64_t buildsBefore = moveTileBuildCount();
    auto cmd = moveLayerContent(*doc, base, 199 * T, 0);
    PE_REQUIRE(cmd != nullptr);
    PE_CHECK_EQ(moveTileBuildCount() - buildsBefore, static_cast<std::uint64_t>(2));
    PE_CHECK_EQ(cmd->touchedTileCount(), static_cast<std::size_t>(2));
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(199 * T + 3, 3), (Rgba8{1, 2, 3, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(3, 3), (Rgba8{0, 0, 0, 0}));

    // The same content dragged far enough that the bounding box alone would have exceeded
    // the old 4096-tile count is still accepted, because two tiles is what it costs.
    auto doc2 = Document::createBlank(Size{4 * T, 4 * T});
    PE_REQUIRE(doc2 != nullptr);
    auto* pl2 = static_cast<PixelLayer*>(doc2->findLayer(doc2->activeLayer()));
    pl2->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{4, 5, 6, 255});
    const std::uint64_t farBefore = moveTileBuildCount();
    auto far = moveLayerContent(*doc2, doc2->activeLayer(), 100 * T, 100 * T);
    PE_REQUIRE(far != nullptr);  // bounding box 101x101 = 10201 tiles, content is 2
    PE_CHECK_EQ(moveTileBuildCount() - farBefore, static_cast<std::uint64_t>(2));
    PE_CHECK_EQ(far->touchedTileCount(), static_cast<std::size_t>(2));
}

PE_TEST(move_budget_counts_bytes_so_a_float_layer_is_bounded_like_one) {
    // A Move's tiles are the layer's own pixels: 256 KB at U8, 1 MB at F32. A flat tile
    // count let a float layer commit four times the memory of an 8-bit layer with the same
    // geometry, so the budget is stated in bytes and the same document refuses at F32 while
    // it is accepted at U8.
    constexpr int T = kTileSize;
    // 24x24 tiles of content: 576 at the source plus 625 at the destination (the shift puts
    // it across one more tile column and row) is 1201 tiles. At U8 that is 315 MB, inside
    // the 1 GiB budget; at F32 it is 1.26 GB, outside it. Same geometry, same tile count.
    const int side = 24 * T;
    auto u8 = Document::createBlank(Size{side, side}, ColorMode::RGB, BitDepth::U8, 96);
    PE_REQUIRE(u8 != nullptr);
    auto* p8 = static_cast<PixelLayer*>(u8->findLayer(u8->activeLayer()));
    p8->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
    p8->tiles().setPixel(side - 1, side - 1, Rgba8{4, 5, 6, 255});

    auto f32 = Document::createBlank(Size{side, side}, ColorMode::RGB, BitDepth::F32, 96);
    PE_REQUIRE(f32 != nullptr);
    auto* pf = static_cast<PixelLayer*>(f32->findLayer(f32->activeLayer()));
    pf->tilesF().setPixel(0, 0, Rgbaf{0.1f, 0.2f, 0.3f, 1.0f});
    pf->tilesF().setPixel(side - 1, side - 1, Rgbaf{0.4f, 0.5f, 0.6f, 1.0f});

    // Same geometry, same tile count, four times the bytes.
    PE_CHECK(p8->contentBounds() == pf->contentBounds());
    PE_CHECK(moveLayerContent(*f32, f32->activeLayer(), 4, 4) == nullptr);
    PE_CHECK(moveLayerContent(*u8, u8->activeLayer(), 4, 4) != nullptr);
}

PE_TEST(move_refuses_content_outside_the_representable_coordinate_range) {
    // The generic bake used to range-check the region on the way past, and moving off that
    // path took the check with it, leaving only the offset bound. Rect::right() is x + width
    // in int, so an out-of-range origin overflows on first use, including inside tilesForRect
    // while computing the budget. Reachable because PixelLayer::tiles() is public and
    // mutable, which is how the tests themselves build fixtures.
    auto doc = Document::createBlank(Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().setPixel(kMaxCanvasDimension + 1000, 5, Rgba8{1, 2, 3, 255});
    PE_CHECK(moveLayerContent(*doc, base, 4, 4) == nullptr);

    // And a source in range whose DESTINATION would leave it is refused too.
    auto ok = Document::createBlank(Size{64, 64});
    PE_REQUIRE(ok != nullptr);
    auto* pl2 = static_cast<PixelLayer*>(ok->findLayer(ok->activeLayer()));
    pl2->tiles().setPixel(kMaxCanvasDimension - 10, 5, Rgba8{1, 2, 3, 255});
    PE_CHECK(moveLayerContent(*ok, ok->activeLayer(), 5000, 0) == nullptr);
}

PE_TEST(move_refusal_explains_what_the_move_actually_declined) {
    // bakeRefusal must NOT be used for a Move: it answers about kMaxFilterPixels, which a
    // Move has not been bounded by since it moved to its own native-depth path. Asked about a
    // 4000x4000 layer it says "over budget, 16 megapixels" for a Move that in fact succeeds,
    // which is worse than saying nothing.
    constexpr int T = kTileSize;
    {
        auto doc = Document::createBlank(Size{4000, 4000});
        PE_REQUIRE(doc != nullptr);
        const LayerId base = doc->activeLayer();
        auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
        pl->tiles().fillRect(Rect{0, 0, 4000, 4000}, Rgba8{1, 2, 3, 255});
        // The move succeeds, so its refusal must be None...
        PE_CHECK(!moveRefusal(*doc, base, 25, 25).isRefusal());
        PE_CHECK(moveLayerContent(*doc, base, 25, 25) != nullptr);
        // ...while bakeRefusal, asked about the same layer, confidently refuses.
        PE_CHECK(bakeRefusal(*doc, base).isRefusal());
    }
    {  // Over the move budget: refused, and the reason names megabytes, not megapixels.
        auto doc = Document::createBlank(Size{70 * T, 70 * T});
        PE_REQUIRE(doc != nullptr);
        const LayerId base = doc->activeLayer();
        auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
        pl->tiles().setPixel(1, 1, Rgba8{1, 2, 3, 255});
        pl->tiles().setPixel(69 * T, 69 * T, Rgba8{4, 5, 6, 255});
        const Refusal why = moveRefusal(*doc, base, 4, 4);
        PE_CHECK(why.isRefusal());
        PE_CHECK(why.code == RefusalCode::OverSizeBudget);
        PE_CHECK(why.explanation.find("MB") != std::string::npos);
        PE_CHECK(moveLayerContent(*doc, base, 4, 4) == nullptr);  // and it really is refused
    }
    {  // An empty layer, and a zero move, are distinct NoEffect cases rather than budget ones.
        auto doc = Document::createBlank(Size{64, 64});
        PE_REQUIRE(doc != nullptr);
        const LayerId base = doc->activeLayer();
        PE_CHECK(moveRefusal(*doc, base, 4, 4).code == RefusalCode::NoEffect);
        static_cast<PixelLayer*>(doc->findLayer(base))->tiles().setPixel(5, 5, Rgba8{1, 2, 3, 255});
        PE_CHECK(moveRefusal(*doc, base, 0, 0).code == RefusalCode::NoEffect);
        PE_CHECK(!moveRefusal(*doc, base, 4, 4).isRefusal());
    }
}

PE_TEST(move_refuses_past_the_memory_budget) {
    // The bound that replaced the area cap is stated in bytes, because what a Move commits
    // is one replacement tile per touched tile at the layer's own pixel size. Just past the
    // budget must still be refused, and refused cleanly rather than by running out of
    // memory. 70x70 tiles at the source plus as many at the destination is about 2.4 GB at
    // 8 bit, well past kMaxMoveBytes.
    constexpr int T = kTileSize;
    auto doc = Document::createBlank(Size{70 * T, 70 * T});
    PE_REQUIRE(doc != nullptr);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().setPixel(1, 1, Rgba8{1, 2, 3, 255});
    pl->tiles().setPixel(69 * T, 69 * T, Rgba8{4, 5, 6, 255});  // 70x70 = 4900 tiles
    PE_CHECK(moveLayerContent(*doc, base, 4, 4) == nullptr);
    PE_CHECK_EQ(pl->tiles().pixel(1, 1), (Rgba8{1, 2, 3, 255}));  // and nothing moved
}

PE_TEST(bucket_fill_floods_contiguous_region) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 8, 16}, Rgba8{255, 255, 255, 255});  // left half white
    pl->tiles().fillRect(Rect{8, 0, 8, 16}, Rgba8{0, 0, 200, 255});      // right half blue

    auto cmd = bucketFill(*doc, base, 2, 2, Rgbaf{1.0f, 0.0f, 0.0f, 1.0f}, 10);  // red into white
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(2, 2), (Rgba8{255, 0, 0, 255}));   // white region -> red
    PE_CHECK_EQ(pl->tiles().pixel(12, 2), (Rgba8{0, 0, 200, 255}));  // blue region untouched

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(2, 2), (Rgba8{255, 255, 255, 255}));  // restored

    PE_CHECK(bucketFill(*doc, base, 999, 999, Rgbaf{}, 0) == nullptr);  // off-canvas seed
}

PE_TEST(gradient_fill_interpolates_along_axis) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));

    // Horizontal black->white gradient across the 16px width.
    auto cmd =
        gradientFill(*doc, base, Point{0, 0}, Point{15, 0}, Rgbaf{0, 0, 0, 1}, Rgbaf{1, 1, 1, 1});
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(pl->tiles().pixel(0, 0).r < 40);    // near black
    PE_CHECK(pl->tiles().pixel(15, 0).r > 215);  // near white
    const int mid = pl->tiles().pixel(8, 0).r;
    PE_CHECK(mid > 110 && mid < 180);  // roughly mid-gray in the middle

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(8, 0).a, static_cast<uint8_t>(0));  // restored transparent

    PE_CHECK(gradientFill(*doc, base, Point{5, 5}, Point{5, 5}, Rgbaf{}, Rgbaf{}) ==
             nullptr);  // zero-length drag
}

PE_TEST(gradient_fill_composites_over_backdrop) {
    // A semi-transparent stop must let the existing pixels show through (straight-alpha Normal),
    // matching bucketFill — not hard-overwrite. Fill the layer opaque red, then run a gradient
    // from transparent (alpha 0, at x=0) to opaque blue (at x=15).
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{255, 0, 0, 255});  // opaque red backdrop

    auto cmd =
        gradientFill(*doc, base, Point{0, 0}, Point{15, 0}, Rgbaf{0, 0, 0, 0}, Rgbaf{0, 0, 1, 1});
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const Rgba8 left = pl->tiles().pixel(0, 0);    // transparent stop -> backdrop shows through
    const Rgba8 right = pl->tiles().pixel(15, 0);  // opaque blue stop -> replaces
    PE_CHECK(left.r > 215 && left.b < 40);         // still red
    PE_CHECK(right.b > 215 && right.r < 40);       // now blue

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(0, 0), (Rgba8{255, 0, 0, 255}));  // backdrop restored
}

PE_TEST(stamp_buffer_composites_and_undoes) {
    auto doc = Document::createBlank(Size{16, 16});  // transparent
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));

    PixelBuffer src(4, 4, Rgba8{255, 0, 0, 255});  // opaque red 4x4
    auto cmd = stampBuffer(*doc, base, Point{2, 3}, src, "Type");
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(2, 3), (Rgba8{255, 0, 0, 255}));    // top-left of the stamp
    PE_CHECK_EQ(pl->tiles().pixel(5, 6), (Rgba8{255, 0, 0, 255}));    // bottom-right (2+3, 3+3)
    PE_CHECK_EQ(pl->tiles().pixel(6, 3).a, static_cast<uint8_t>(0));  // just outside -> untouched

    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(2, 3).a, static_cast<uint8_t>(0));  // restored

    // A semi-transparent source composites over an opaque backdrop (straight-alpha Normal).
    pl->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{0, 0, 255, 255});  // opaque blue
    PixelBuffer half(2, 2, Rgba8{255, 0, 0, 128});                    // 50% red
    auto cmd2 = stampBuffer(*doc, base, Point{0, 0}, half, "Type");
    PE_CHECK(cmd2 != nullptr);
    doc->history().push(std::move(cmd2));
    const Rgba8 blended = pl->tiles().pixel(0, 0);
    PE_CHECK(blended.r > 100 && blended.b > 100);  // red over blue -> a blend of both

    // Degenerate / unsupported inputs return nullptr.
    PE_CHECK(stampBuffer(*doc, base, Point{0, 0}, PixelBuffer{}, "Type") == nullptr);  // empty src
    PE_CHECK(stampBuffer(*doc, kNoLayer, Point{0, 0}, src, "Type") == nullptr);  // no such layer
}

PE_TEST(stamp_buffer_honors_selection) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));

    Selection sel;
    sel.selectRect(Rect{0, 0, 2, 2});              // only the top-left 2x2 is selected
    PixelBuffer src(4, 4, Rgba8{0, 255, 0, 255});  // opaque green 4x4 at the origin
    auto cmd = stampBuffer(*doc, base, Point{0, 0}, src, "Type", &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(pl->tiles().pixel(0, 0), (Rgba8{0, 255, 0, 255}));  // inside selection -> stamped
    PE_CHECK_EQ(pl->tiles().pixel(3, 3).a,
                static_cast<uint8_t>(0));  // outside selection -> untouched
}

PE_TEST(stamp_buffer_16bit_layer_and_negative_origin) {
    // 16-bit-depth layer: the stamp routes through the native U16 store.
    auto doc = Document::createBlank(Size{16, 16}, ColorMode::RGB, BitDepth::U16);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    PixelBuffer src(4, 4, Rgba8{255, 0, 0, 255});
    auto cmd = stampBuffer(*doc, base, Point{0, 0}, src, "Type");
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const Rgba16 px = pl->tiles16().pixel(1, 1);
    PE_CHECK(px.r > 60000 && px.g < 5000 && px.a > 60000);  // opaque red, widened to 16-bit

    // Negative origin: a 4x4 stamp at (-2,-2) lands its lower-right quarter at doc (0,0)..(1,1).
    auto doc2 = Document::createBlank(Size{16, 16});
    const LayerId b2 = doc2->activeLayer();
    auto* pl2 = static_cast<PixelLayer*>(doc2->findLayer(b2));
    auto cmd2 = stampBuffer(*doc2, b2, Point{-2, -2}, src, "Type");
    PE_CHECK(cmd2 != nullptr);
    doc2->history().push(std::move(cmd2));
    PE_CHECK_EQ(pl2->tiles().pixel(0, 0), (Rgba8{255, 0, 0, 255}));  // covered by the stamp
    doc2->history().undo();
    PE_CHECK_EQ(pl2->tiles().pixel(0, 0).a, static_cast<uint8_t>(0));  // restored
}

namespace {
std::unique_ptr<Document> docWithRect(Rect fill, Rgba8 color, Size canvas) {
    auto doc = Document::createBlank(canvas);
    static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()))->tiles().fillRect(fill, color);
    return doc;
}
}  // namespace

PE_TEST(transform_identity_is_noop) {
    // Identity inverse-maps each dest pixel to its own integer source coord, reproducing every
    // pixel exactly — so nothing changes and no command (no phantom undo entry) is produced.
    auto doc = docWithRect(Rect{10, 10, 20, 20}, Rgba8{200, 100, 50, 255}, Size{64, 64});
    PE_CHECK(transformLayerContent(*doc, doc->activeLayer(), Affine2D{}) == nullptr);
}

PE_TEST(transform_integer_translate_matches_move) {
    // An integer translation samples at integer coords (no resampling error), so it must equal
    // moveLayerContent pixel-for-pixel.
    auto d1 = docWithRect(Rect{8, 8, 16, 16}, Rgba8{50, 150, 250, 255}, Size{64, 64});
    auto d2 = docWithRect(Rect{8, 8, 16, 16}, Rgba8{50, 150, 250, 255}, Size{64, 64});
    d1->history().push(moveLayerContent(*d1, d1->activeLayer(), 12, 9));
    d2->history().push(
        transformLayerContent(*d2, d2->activeLayer(), Affine2D::translation(12.0, 9.0)));
    auto* p1 = static_cast<PixelLayer*>(d1->findLayer(d1->activeLayer()));
    auto* p2 = static_cast<PixelLayer*>(d2->findLayer(d2->activeLayer()));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) PE_CHECK_EQ(p1->tiles().pixel(x, y), p2->tiles().pixel(x, y));
    }
}

PE_TEST(transform_scale_2x_grows_content) {
    auto doc = docWithRect(Rect{0, 0, 16, 16}, Rgba8{255, 0, 0, 255}, Size{128, 128});
    const LayerId base = doc->activeLayer();
    doc->history().push(transformLayerContent(*doc, base, Affine2D::scaling(2.0, 2.0)));
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    PE_CHECK(pl->tiles().pixel(20, 20).a > 200);  // inside the doubled 0..32 region
    PE_CHECK_EQ(pl->tiles().pixel(40, 40).a,
                static_cast<uint8_t>(0));  // beyond the dst, transparent
}

PE_TEST(transform_rejects_degenerate_nonfinite_and_huge) {
    auto doc = docWithRect(Rect{0, 0, 16, 16}, Rgba8{255, 0, 0, 255}, Size{32, 32});
    const LayerId base = doc->activeLayer();
    PE_CHECK(transformLayerContent(*doc, base, Affine2D::scaling(0.0, 0.0)) ==
             nullptr);  // singular
    Affine2D nanT;
    nanT.m00 = std::nan("");
    PE_CHECK(transformLayerContent(*doc, base, nanT) == nullptr);  // non-finite -> no UB
    PE_CHECK(transformLayerContent(*doc, base, Affine2D::translation(1e9, 0.0)) ==
             nullptr);  // off-range
}

PE_TEST(transform_empty_layer_is_null) {
    auto doc = Document::createBlank(Size{32, 32});
    PE_CHECK(transformLayerContent(*doc, doc->activeLayer(), Affine2D::scaling(2.0, 2.0)) ==
             nullptr);
}

PE_TEST(transform_undo_restores_exact) {
    auto doc = docWithRect(Rect{10, 10, 20, 20}, Rgba8{30, 60, 90, 255}, Size{64, 64});
    const LayerId base = doc->activeLayer();
    doc->history().push(transformLayerContent(*doc, base, Affine2D::rotation(0.5)));
    doc->history().undo();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    PE_CHECK_EQ(pl->tiles().pixel(15, 15), (Rgba8{30, 60, 90, 255}));  // exact restore
    PE_CHECK_EQ(pl->tiles().pixel(0, 0).a, static_cast<uint8_t>(0));
}

PE_TEST(transform_preserves_superwhite_on_f32) {
    // The resample path is depth-generic; on an F32 layer it must preserve HDR (>1.0) values —
    // premultiply/unpremultiply don't clamp. A 2x scale takes the bilinear resample path (not the
    // integer-translate fast path).
    auto doc = Document::createBlank(Size{64, 64}, ColorMode::RGB, BitDepth::F32);
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tilesF().fillRect(Rect{0, 0, 16, 16}, Rgbaf{4.0f, 0.0f, 0.0f, 1.0f});  // super-white red
    doc->history().push(transformLayerContent(*doc, base, Affine2D::scaling(2.0, 2.0)));
    PE_CHECK(pl->tilesF().pixel(10, 10).r > 3.0f);  // HDR survived the resample (not clamped to 1)
}

PE_TEST(filter_gaussian_tiny_positive_sigma_is_safe) {
    // A tiny but positive, finite sigma passes the non-finite guard, and squaring it
    // underflows to zero: the kernel becomes all NaN and the region comes out fully
    // transparent instead of very slightly blurred. Selection::feather clamps for
    // exactly this reason; the filter's kernel builder did not.
    auto src = grayRow({0.0f, 1.0f, 0.0f, 1.0f, 0.0f});
    std::vector<Rgbaf> dst(src.size());

    for (const float sigma : {1e-30f, 1e-20f, 1e-8f, 1e-3f}) {
        gaussianBlur(src, dst, 5, 1, sigma);
        for (const Rgbaf& p : dst) {
            PE_CHECK(std::isfinite(p.r));
            PE_CHECK(std::isfinite(p.a));
        }
        // Well below one pixel of blur, so the result must still resemble the input
        // rather than collapsing to transparent black.
        for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_NEAR(dst[i].r, src[i].r);
    }
}

PE_TEST(bake_refusal_agrees_with_what_the_bake_actually_does) {
    // bakeRefusal re-evaluates the preconditions rather than reporting from the call that
    // declined, so the risk it carries is DRIFT: the predicate saying one thing while the
    // bake does another. This pins them together over every condition, which is the whole
    // reason the predicate is trustworthy enough to explain a refusal to a user.
    const auto noop = [](std::span<Rgbaf>, int, int) {};
    const auto bakes = [&noop](Document& doc, LayerId id) {
        return bakePixelEdit(doc, id, "Probe", noop, nullptr) != nullptr;
    };
    const auto refused = [](Document& doc, LayerId id) { return bakeRefusal(doc, id).isRefusal(); };

    // No such layer.
    {
        auto doc = Document::createBlank(Size{32, 32});
        PE_CHECK_EQ(bakes(*doc, kNoLayer), false);
        PE_CHECK_EQ(refused(*doc, kNoLayer), true);
        PE_CHECK(bakeRefusal(*doc, kNoLayer).code == RefusalCode::NoActiveLayer);
    }
    // Empty pixel layer: no content to edit.
    {
        auto doc = Document::createBlank(Size{32, 32});
        const LayerId id = doc->activeLayer();
        PE_CHECK_EQ(bakes(*doc, id), false);
        PE_CHECK_EQ(refused(*doc, id), true);
        PE_CHECK(bakeRefusal(*doc, id).code == RefusalCode::NoEffect);
    }
    // A pixel layer with content: both agree it can proceed.
    {
        auto doc = Document::createBlank(Size{32, 32});
        const LayerId id = doc->activeLayer();
        static_cast<PixelLayer*>(doc->findLayer(id))
            ->tiles()
            .fillRect(Rect{0, 0, 32, 32}, Rgba8{5, 6, 7, 255});
        PE_CHECK_EQ(refused(*doc, id), false);
        // A no-op transform produces no deltas, so the command is null for a reason the
        // preconditions do not cover. That is exactly the residual gap bakeRefusal
        // documents, and the shell handles it by saying the settings changed nothing
        // rather than inventing a cause.
        const bool wroteSomething = bakes(*doc, id);
        PE_CHECK(!wroteSomething);
    }
    // A pixel layer whose content really changes: the bake succeeds and nothing refuses.
    {
        auto doc = Document::createBlank(Size{32, 32});
        const LayerId id = doc->activeLayer();
        static_cast<PixelLayer*>(doc->findLayer(id))
            ->tiles()
            .fillRect(Rect{0, 0, 32, 32}, Rgba8{5, 6, 7, 255});
        auto cmd = bakePixelEdit(
            *doc, id, "Probe",
            [](std::span<Rgbaf> img, int, int) {
                for (Rgbaf& p : img) p.r = 1.0f;
            },
            nullptr);
        PE_CHECK(cmd != nullptr);
        PE_CHECK_EQ(refused(*doc, id), false);
    }
    // Non-pixel active layer.
    {
        auto doc = Document::createBlank(Size{32, 32});
        auto adj = std::make_unique<AdjustmentLayer>(std::make_unique<Invert>(), "Invert");
        const LayerId adjId = adj->id();
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));
        PE_CHECK_EQ(bakes(*doc, adjId), false);
        PE_CHECK_EQ(refused(*doc, adjId), true);
        PE_CHECK(bakeRefusal(*doc, adjId).code == RefusalCode::LayerNotPixel);
    }
    // Over the filter budget.
    {
        const int side = 5000;  // 25 MP, over the 16 MP cap
        auto doc = Document::createBlank(Size{side, side});
        const LayerId id = doc->activeLayer();
        static_cast<PixelLayer*>(doc->findLayer(id))
            ->tiles()
            .fillRect(Rect{0, 0, side, side}, Rgba8{5, 6, 7, 255});
        PE_CHECK_EQ(bakes(*doc, id), false);
        PE_CHECK_EQ(refused(*doc, id), true);
        const Refusal r = bakeRefusal(*doc, id);
        PE_CHECK(r.code == RefusalCode::OverSizeBudget);
        PE_CHECK(!r.fixableByState);  // no choice of layer makes it fit
        PE_CHECK(!r.context.empty());
    }
}
