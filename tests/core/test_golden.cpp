// Golden-image regression tests for the subsystems that draw.
//
// See tests/golden.hpp for the harness, the tolerance model, and how to regenerate.
//
// Every case here asserts a STRUCTURAL property as well as comparing against its
// reference. That pairing is the point: a golden on its own promotes whatever the code did
// on the day it was generated into the specification, so a reference captured from a bug
// would enshrine the bug. The structural half is also the only half that runs in the
// no-optional-deps lane, which has no PNG codec.

#include "golden.hpp"
#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Brush.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pe;

namespace {

constexpr int kW = 128;
constexpr int kH = 96;
const Rect kCanvas{0, 0, kW, kH};

// A deterministic test image with structure at several scales: a diagonal ramp, a hard
// vertical edge, and a checkerboard block. Filters have something to actually do to each.
PixelBuffer testImage() {
    PixelBuffer img(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            auto r = static_cast<std::uint8_t>((x * 255) / (kW - 1));
            auto g = static_cast<std::uint8_t>((y * 255) / (kH - 1));
            std::uint8_t b = 64;
            if (x >= kW / 2) b = 200;                      // hard vertical edge
            if (x >= 16 && x < 48 && y >= 16 && y < 48) {  // checkerboard block
                const bool on = ((x / 4) + (y / 4)) % 2 == 0;
                r = on ? 240 : 20;
                g = on ? 20 : 240;
            }
            img.set(x, y, Rgba8{r, g, b, 255});
        }
    }
    return img;
}

std::unique_ptr<Document> docWithTestImage(LayerId& out) {
    auto doc = Document::createBlank(Size{kW, kH});
    out = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(out));
    const PixelBuffer src = testImage();
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) pl->tiles().setPixel(x, y, src.at(x, y));
    }
    return doc;
}

PixelBuffer flatten(Document& doc) {
    return doc.compositeImage();
}

// Mean absolute difference between horizontally adjacent pixels: a blur lowers it, a
// sharpen raises it. Cheap, and independent of the reference.
double horizontalContrast(const PixelBuffer& img) {
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 1; x < img.width(); ++x) {
            const Rgba8 a = img.at(x - 1, y);
            const Rgba8 b = img.at(x, y);
            sum += std::abs(static_cast<int>(a.r) - static_cast<int>(b.r));
            sum += std::abs(static_cast<int>(a.g) - static_cast<int>(b.g));
            sum += std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
            n += 3;
        }
    }
    return n > 0 ? sum / n : 0.0;
}

}  // namespace

PE_TEST(golden_gaussian_blur) {
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    const double before = horizontalContrast(flatten(*doc));

    auto cmd = applyFilter(*doc, id, GaussianBlurFilter(2.5f));
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    // Structural: a blur must reduce local contrast. A reference captured from a broken
    // blur (an identity, or a NaN wipe) could not satisfy this.
    PE_CHECK(horizontalContrast(out) < before * 0.75);
#ifdef PHOTOEDIT_HAVE_PNG
    // Tolerant: a separable or running-sum rewrite may shift rounding by an LSB, and
    // blocking that is not what this test is for.
    PE_CHECK(pe_golden::checkGolden("gaussian_blur", out, pe_golden::kKernelRewrite));
#endif
}

PE_TEST(golden_box_blur) {
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    const double before = horizontalContrast(flatten(*doc));
    auto cmd = applyFilter(*doc, id, BoxBlurFilter(3));
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    PE_CHECK(horizontalContrast(out) < before * 0.75);
#ifdef PHOTOEDIT_HAVE_PNG
    // This is the case the running-sum optimization would change: it exists so that
    // rewrite can be shown to preserve the result.
    PE_CHECK(pe_golden::checkGolden("box_blur", out, pe_golden::kKernelRewrite));
#endif
}

PE_TEST(golden_median_filter) {
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    const double before = horizontalContrast(flatten(*doc));
    auto cmd = applyFilter(*doc, id, MedianFilter(2));
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    // The characteristic property of a median is that it removes outliers WITHOUT
    // softening edges, which is what separates it from a blur. At radius 2 the 5x5 window
    // spans about two and a half checker cells, so the checkerboard largely survives; the
    // reference image shows that, and claiming otherwise here would be a comment the
    // picture contradicts.
    PE_CHECK(horizontalContrast(out) < before);

    // Compare the edge step against a Gaussian of comparable reach: the median must keep
    // it far crisper. This is the assertion that a wrong reference could not satisfy.
    LayerId blurId = kNoLayer;
    auto blurDoc = docWithTestImage(blurId);
    auto blurCmd = applyFilter(*blurDoc, blurId, GaussianBlurFilter(2.0f));
    PE_CHECK(blurCmd != nullptr);
    blurDoc->history().push(std::move(blurCmd));
    const PixelBuffer blurred = flatten(*blurDoc);

    // Measured across the edge itself: the source steps from 64 to 200 at x == kW/2, so a
    // filter that preserves edges keeps the full 136 there while any blur spreads it out.
    const auto step = [](const PixelBuffer& img) {
        return static_cast<int>(img.at(kW / 2, kH - 4).b) -
               static_cast<int>(img.at(kW / 2 - 1, kH - 4).b);
    };
    PE_CHECK_EQ(step(out), 136);   // the median leaves the edge untouched
    PE_CHECK(step(blurred) < 40);  // the blur does not
    PE_CHECK(step(out) > step(blurred) * 2);
#ifdef PHOTOEDIT_HAVE_PNG
    // A constant-time median must return the same value, not merely a similar one: the
    // median of a set is exact, so a rewrite has no rounding freedom to spend.
    PE_CHECK(pe_golden::checkGolden("median_filter", out, pe_golden::kExact));
#endif
}

PE_TEST(golden_sharpen_filter) {
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    const double before = horizontalContrast(flatten(*doc));
    auto cmd = applyFilter(*doc, id, SharpenFilter(1.0f));
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    PE_CHECK(horizontalContrast(out) > before);  // sharpen raises local contrast
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("sharpen_filter", out, pe_golden::kKernelRewrite));
#endif
}

PE_TEST(golden_brush_stroke) {
    // The brush engine draws through dab coverage, spacing, hardness, flow and opacity.
    // None of that had a picture of its output committed anywhere.
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    BrushSettings b;
    b.diameter = 17.0f;
    b.hardness = 0.35f;  // a soft edge, so the falloff is part of the picture
    b.opacity = 0.8f;
    b.flow = 0.5f;
    b.spacing = 0.2f;

    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 20; ++i) {
        const auto t = static_cast<float>(i);
        pts.push_back(StrokePoint{Vec2{12.0f + t * 5.2f, 20.0f + 18.0f * std::sin(t * 0.35f)},
                                  0.35f + 0.03f * t});
    }
    auto cmd = paintStroke(*doc, id, b, Rgbaf{0.05f, 0.15f, 0.95f, 1.0f}, pts, nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    // Structural: the stroke deposited blue along its path and left the far corner alone.
    PE_CHECK(out.at(12, 20).b > 120);
    PE_CHECK_EQ(out.at(kW - 2, kH - 2), testImage().at(kW - 2, kH - 2));
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("brush_stroke", out, pe_golden::kExact));
#endif
}

PE_TEST(golden_gradient_fill) {
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    auto cmd = gradientFill(*doc, id, Point{8, 8}, Point{116, 84}, Rgbaf{1.0f, 0.9f, 0.1f, 1.0f},
                            Rgbaf{0.1f, 0.2f, 0.8f, 1.0f}, nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    // Structural: monotonic along the gradient axis, from the near stop to the far one.
    PE_CHECK(out.at(10, 10).r > out.at(60, 46).r);
    PE_CHECK(out.at(60, 46).r > out.at(112, 80).r);
    PE_CHECK(out.at(112, 80).b > out.at(10, 10).b);
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("gradient_fill", out, pe_golden::kExact));
#endif
}

PE_TEST(golden_adjustment_stack) {
    // A non-destructive adjustment layer over the image, masked, so the composite exercises
    // the adjustment path, the mask path and the blend in one picture.
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    auto adj =
        std::make_unique<AdjustmentLayer>(std::make_unique<BrightnessContrast>(0.15f, 0.35f), "BC");
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{0, 0, kW / 2, kH}, MaskBuffer::kClear);  // left half unadjusted
    adj->setMask(std::move(mask));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));
    const PixelBuffer out = flatten(*doc);

    // Structural: the masked half matches the original, the revealed half does not.
    const PixelBuffer src = testImage();
    PE_CHECK_EQ(out.at(4, kH - 4), src.at(4, kH - 4));
    PE_CHECK(out.at(kW - 4, kH - 4) != src.at(kW - 4, kH - 4));
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("adjustment_stack", out, pe_golden::kExact));
#endif
}

PE_TEST(golden_blend_modes_over_a_gradient) {
    // The compositor at a realistic size with a non-trivial blend and partial opacity. The
    // existing golden_compositor_* tests fill an 8x8 buffer with one colour, which cannot
    // catch anything positional.
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    auto top = std::make_unique<SolidColorLayer>(Rgba8{60, 200, 120, 255}, kCanvas);
    top->setBlendMode(BlendMode::Overlay);
    top->setOpacity(0.6f);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(top));
    const PixelBuffer out = flatten(*doc);

    // Structural: Overlay preserves the light/dark ordering of the backdrop it acts on.
    const PixelBuffer src = testImage();
    PE_CHECK((src.at(100, 8).r > src.at(4, 8).r) == (out.at(100, 8).r > out.at(4, 8).r));
    PE_CHECK(out.at(4, 8) != src.at(4, 8));  // and it did something
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("blend_overlay", out, pe_golden::kExact));
#endif
}

PE_TEST(golden_blur_brush) {
    // The Blur BRUSH, distinct from the whole-layer Gaussian filter above: it convolves
    // over the stroke's region and blends by coverage. Its output is now well defined
    // (independent of where else the stroke went), so it can have a reference at all.
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    const double before = horizontalContrast(flatten(*doc));

    BrushSettings b;
    b.diameter = 21.0f;
    b.hardness = 0.7f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 14; ++i) {
        const auto t = static_cast<float>(i);
        pts.push_back(
            StrokePoint{Vec2{14.0f + t * 7.0f, 30.0f + 10.0f * std::sin(t * 0.5f)}, 1.0f});
    }
    auto cmd = blurStroke(*doc, id, b, pts, nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    PE_CHECK(horizontalContrast(out) < before);                           // it softened something
    PE_CHECK_EQ(out.at(kW - 2, kH - 2), testImage().at(kW - 2, kH - 2));  // and only locally
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("blur_brush", out, pe_golden::kKernelRewrite));
#endif
}

PE_TEST(golden_sharpen_brush) {
    LayerId id = kNoLayer;
    auto doc = docWithTestImage(id);
    const double before = horizontalContrast(flatten(*doc));

    BrushSettings b;
    b.diameter = 21.0f;
    b.hardness = 0.7f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 14; ++i) {
        const auto t = static_cast<float>(i);
        // Placed across the checkerboard and the hard edge, like the blur case: a stroke
        // through smooth gradient would make a reference that barely shows the operation.
        pts.push_back(
            StrokePoint{Vec2{14.0f + t * 7.0f, 30.0f + 10.0f * std::sin(t * 0.5f)}, 1.0f});
    }
    auto cmd = sharpenStroke(*doc, id, b, pts, nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const PixelBuffer out = flatten(*doc);

    PE_CHECK(horizontalContrast(out) > before);
    PE_CHECK_EQ(out.at(kW - 2, kH - 2), testImage().at(kW - 2, kH - 2));
#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(pe_golden::checkGolden("sharpen_brush", out, pe_golden::kKernelRewrite));
#endif
}
