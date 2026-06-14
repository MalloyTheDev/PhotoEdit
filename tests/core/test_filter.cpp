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
