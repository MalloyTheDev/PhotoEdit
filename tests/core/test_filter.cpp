#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <limits>
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
