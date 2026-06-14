#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

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

PE_TEST(brush_refuses_high_depth_layer_for_now) {
    auto doc = Document::createBlank(Size{16, 16}, ColorMode::RGB, BitDepth::U16);
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts{StrokePoint{Vec2{8.0f, 8.0f}, 1.0f}};
    // Brushing a 16-bit layer is unsupported for now and safely no-ops (nullptr).
    PE_CHECK(paintStroke(*doc, base, BrushSettings{}, Rgbaf{1, 0, 0, 1}, pts) == nullptr);
}
