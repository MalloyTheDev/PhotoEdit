#include "pe/core/Compositor.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstdlib>
#include <memory>
#include <vector>

using namespace pe;

namespace {

constexpr int kW = 8;
constexpr int kH = 8;
const Rect kCanvas{0, 0, kW, kH};

constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kGreen{0, 255, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};
constexpr Rgba8 kWhite{255, 255, 255, 255};
constexpr Rgba8 kGray{128, 128, 128, 255};

std::unique_ptr<Layer> solid(Rgba8 c, Rect bounds = kCanvas) {
    return std::make_unique<SolidColorLayer>(c, bounds);
}

bool near8(Rgba8 a, Rgba8 b, int tol = 1) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol && d(a.a, b.a) <= tol;
}

}  // namespace

PE_TEST(composite_empty_stack_is_transparent) {
    std::vector<std::unique_ptr<Layer>> stack;
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK_EQ(img.width(), kW);
    PE_CHECK_EQ(img.at(0, 0), (Rgba8{0, 0, 0, 0}));
    PE_CHECK_EQ(img.at(7, 7), (Rgba8{0, 0, 0, 0}));
}

PE_TEST(composite_single_opaque) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kRed));
    PE_CHECK(near8(img.at(7, 7), kRed));
}

PE_TEST(composite_normal_opaque_top_wins) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));   // bottom
    stack.push_back(solid(kBlue));  // top, opaque
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(4, 4), kBlue));
}

PE_TEST(composite_opacity_50_normal) {
    // 50% blue over opaque red => (0.5*blue + 0.5*red) = (128,0,128).
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setOpacity(0.5f);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), Rgba8{128, 0, 128, 255}));
}

PE_TEST(composite_multiply_red_green_is_black) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kGreen);
    top->setBlendMode(BlendMode::Multiply);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), Rgba8{0, 0, 0, 255}));
}

PE_TEST(composite_screen_red_green_is_yellow) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kGreen);
    top->setBlendMode(BlendMode::Screen);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), Rgba8{255, 255, 0, 255}));
}

PE_TEST(composite_multiply_white_gray_is_gray) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kWhite));
    auto top = solid(kGray);
    top->setBlendMode(BlendMode::Multiply);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kGray));
}

PE_TEST(composite_hidden_layer_skipped) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setVisible(false);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kRed));  // blue hidden
}

PE_TEST(composite_group_opacity_matches_inline) {
    // A group containing an opaque blue, group at 50% over red == 50% blue/red.
    auto group = std::make_unique<GroupLayer>("g");
    group->addChild(solid(kBlue));
    group->setOpacity(0.5f);

    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    stack.push_back(std::move(group));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), Rgba8{128, 0, 128, 255}));
}

PE_TEST(composite_nested_groups) {
    // Group within a group, innermost opaque green, over red => green.
    auto inner = std::make_unique<GroupLayer>("inner");
    inner->addChild(solid(kGreen));
    auto outer = std::make_unique<GroupLayer>("outer");
    outer->addChild(std::move(inner));

    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    stack.push_back(std::move(outer));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(3, 3), kGreen));
}

PE_TEST(composite_partial_bounds_fill) {
    // A blue solid confined to the left half over a red background.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    stack.push_back(solid(kBlue, Rect{0, 0, 4, kH}));  // left half only
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kBlue));  // inside the blue rect
    PE_CHECK(near8(img.at(5, 0), kRed));   // outside it -> red shows
}

PE_TEST(composite_fill_opacity_applies) {
    // Fill opacity scales content (no effects in M1), like opacity: 50% blue
    // over red => purple, same as the opacity case.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setFillOpacity(0.5f);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), Rgba8{128, 0, 128, 255}));
}

PE_TEST(composite_oversized_canvas_returns_empty) {
    // The whole-image path refuses canvases above the megapixel budget rather
    // than eagerly allocating gigabytes (tile viewport handles large docs).
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed, Rect{0, 0, 100000, 100000}));
    PixelBuffer img = compositeToImage(stack, Rect{0, 0, 100000, 100000});
    PE_CHECK(img.isEmpty());
}

PE_TEST(composite_clipping_confines_to_base) {
    // A clipped red layer (full canvas) over a white base that covers only the
    // left half shows only where the base has coverage.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kWhite, Rect{0, 0, 4, kH}));  // base: left half
    auto clip = solid(kRed);                            // full canvas, clipped
    clip->setClipped(true);
    stack.push_back(std::move(clip));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kRed));  // left: clipped onto base
    PE_CHECK_EQ(img.at(6, 0).a, 0);       // right: no base coverage -> clipped out
}

PE_TEST(composite_clipping_hidden_base_hides_run) {
    // [blue base, white base (hidden), red clipped]. The red clips to the white
    // base; because white is hidden, the red run is hidden too (not re-clipped to
    // blue). Result = blue.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kBlue));
    auto base = solid(kWhite);
    base->setVisible(false);
    stack.push_back(std::move(base));
    auto clip = solid(kRed);
    clip->setClipped(true);
    stack.push_back(std::move(clip));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kBlue));  // red hidden with its base -> blue shows
}

PE_TEST(composite_clipping_without_base_is_normal) {
    // A clipped layer with nothing below it behaves like a normal layer.
    std::vector<std::unique_ptr<Layer>> stack;
    auto clip = solid(kRed);
    clip->setClipped(true);
    stack.push_back(std::move(clip));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kRed));
    PE_CHECK(near8(img.at(7, 7), kRed));
}

PE_TEST(composite_zero_opacity_contributes_nothing) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setOpacity(0.0f);
    stack.push_back(std::move(top));
    PixelBuffer img = compositeToImage(stack, kCanvas);
    PE_CHECK(near8(img.at(0, 0), kRed));
}

PE_TEST(composite_float_matches_8bit_after_quantize) {
    // The float output, quantized to 8-bit, must equal the 8-bit output exactly:
    // both run the identical tile logic; only the final write differs.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setOpacity(0.5f);
    stack.push_back(std::move(top));

    PixelBuffer i8 = compositeToImage(stack, kCanvas);
    PixelBufferF iF = compositeToImageF(stack, kCanvas);
    PE_CHECK_EQ(iF.width(), kW);
    PE_CHECK_EQ(iF.height(), kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            PE_CHECK(near8(toRgba8(iF.at(x, y)), i8.at(x, y)));
        }
    }
}

PE_TEST(composite_float_preserves_sub_8bit_precision) {
    // 50% blue over red composites to linear 0.5 in float — exactly 0.5, not the
    // 8-bit-quantized 128/255 (~0.50196). This is the banding-defense the float
    // path exists for.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setOpacity(0.5f);
    stack.push_back(std::move(top));
    PixelBufferF iF = compositeToImageF(stack, kCanvas);
    const Rgbaf c = iF.at(0, 0);
    PE_CHECK_NEAR(c.r, 0.5f);  // exact float midpoint, no 8-bit rounding
    PE_CHECK_NEAR(c.b, 0.5f);
    PE_CHECK_NEAR(c.a, 1.0f);
}

PE_TEST(composite_float_empty_stack_transparent) {
    std::vector<std::unique_ptr<Layer>> stack;
    PixelBufferF iF = compositeToImageF(stack, kCanvas);
    PE_CHECK_NEAR(iF.at(0, 0).a, 0.0f);
    PE_CHECK_NEAR(iF.at(7, 7).r, 0.0f);
}

PE_TEST(composite_float_oversized_returns_empty) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed, Rect{0, 0, 100000, 100000}));
    PixelBufferF iF = compositeToImageF(stack, Rect{0, 0, 100000, 100000});
    PE_CHECK(iF.isEmpty());
}

PE_TEST(composite_16bit_matches_8bit_after_narrow) {
    // The 16-bit output, narrowed back to 8-bit, equals the 8-bit output: both run
    // the identical tile logic; 16-bit just keeps more precision per channel.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setOpacity(0.5f);
    stack.push_back(std::move(top));

    PixelBuffer i8 = compositeToImage(stack, kCanvas);
    PixelBuffer16 i16 = compositeToImage16(stack, kCanvas);
    PE_CHECK_EQ(i16.width(), kW);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            PE_CHECK(near8(to8(i16.at(x, y)), i8.at(x, y)));
        }
    }
}

PE_TEST(composite_16bit_preserves_sub_8bit_precision) {
    // 50% blend -> exactly 0.5 -> 16-bit 32768, which is NOT a multiple of 257
    // (the 8-bit grid), proving sub-8-bit precision is retained.
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed));
    auto top = solid(kBlue);
    top->setOpacity(0.5f);
    stack.push_back(std::move(top));
    PixelBuffer16 i16 = compositeToImage16(stack, kCanvas);
    const Rgba16 c = i16.at(0, 0);
    PE_CHECK_EQ(c.r, static_cast<uint16_t>(32768));  // 0.5 -> round(0.5*65535)+... = 32768
    PE_CHECK_EQ(c.b, static_cast<uint16_t>(32768));
    PE_CHECK_EQ(c.a, static_cast<uint16_t>(65535));
}

PE_TEST(composite_16bit_oversized_returns_empty) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(solid(kRed, Rect{0, 0, 100000, 100000}));
    PixelBuffer16 i16 = compositeToImage16(stack, Rect{0, 0, 100000, 100000});
    PE_CHECK(i16.isEmpty());
}
