#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace pe;

namespace {
constexpr int kW = 8;
constexpr int kH = 8;
const Rect kCanvas{0, 0, kW, kH};
constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kCyan{0, 255, 255, 255};

bool near8(Rgba8 a, Rgba8 b, int tol = 1) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol && d(a.a, b.a) <= tol;
}

Rgbaf applyOne(const Adjustment& adj, Rgbaf in) {
    std::vector<Rgbaf> px{in};
    adj.apply(px);
    return px[0];
}

// Composite a red solid with `adj` as an adjustment layer above it.
PixelBuffer renderAdjusted(std::unique_ptr<Adjustment> adj, std::unique_ptr<Mask> mask = nullptr,
                           float opacity = 1.0f) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(std::make_unique<SolidColorLayer>(kRed, kCanvas));
    auto adjLayer = std::make_unique<AdjustmentLayer>(std::move(adj), "Adj");
    if (mask) adjLayer->setMask(std::move(mask));
    adjLayer->setOpacity(opacity);
    stack.push_back(std::move(adjLayer));
    return compositeToImage(stack, kCanvas);
}
}  // namespace

PE_TEST(adjustment_brightness_contrast) {
    Rgbaf out = applyOne(BrightnessContrast(0.5f, 0.0f), Rgbaf{0.4f, 0.4f, 0.4f, 1.0f});
    PE_CHECK_NEAR(out.r, 0.9f);  // 0.4 + 0.5
    // Contrast pushes away from mid-gray.
    Rgbaf c = applyOne(BrightnessContrast(0.0f, 1.0f), Rgbaf{0.75f, 0.75f, 0.75f, 1.0f});
    PE_CHECK_NEAR(c.r, 1.0f);  // (0.75-0.5)*2 + 0.5 = 1.0
}

PE_TEST(adjustment_levels_identity_and_gamma) {
    Levels identity;
    PE_CHECK_NEAR(applyOne(identity, Rgbaf{0.3f, 0.6f, 0.9f, 1.0f}).g, 0.6f);

    Levels gamma;
    gamma.setGamma(2.0f);
    PE_CHECK_NEAR(gamma.mapChannel(0.5f), 0.70710677f);  // pow(0.5, 0.5)
}

PE_TEST(adjustment_curves_identity_and_map) {
    Curves identity;  // default straight line
    PE_CHECK_NEAR(identity.evalCurve(0.42f), 0.42f);

    Curves c;
    c.setPoints({{0.0f, 0.0f}, {0.5f, 0.25f}, {1.0f, 1.0f}});
    PE_CHECK_NEAR(c.evalCurve(0.5f), 0.25f);
    PE_CHECK_NEAR(c.evalCurve(0.25f), 0.125f);  // halfway up the first segment
}

PE_TEST(adjustment_curves_baked_matches_reference) {
    Curves c;
    c.setPoints({{0.0f, 0.1f}, {0.5f, 0.7f}, {1.0f, 0.9f}});
    for (float v = 0.0f; v <= 1.0f; v += 0.1f) {
        Rgbaf out = applyOne(c, Rgbaf{v, v, v, 1.0f});
        // The 256-entry LUT linearly interpolates across the curve's kinks, so a
        // small bounded error vs. the analytic value is expected (stated tolerance).
        PE_CHECK(std::fabs(out.r - c.evalCurve(v)) < 0.01f);
    }
}

PE_TEST(adjustment_invert) {
    Rgbaf out = applyOne(Invert{}, Rgbaf{0.2f, 0.5f, 0.8f, 1.0f});
    PE_CHECK_NEAR(out.r, 0.8f);
    PE_CHECK_NEAR(out.g, 0.5f);
    PE_CHECK_NEAR(out.b, 0.2f);
}

PE_TEST(adjustment_skips_transparent) {
    Rgbaf out = applyOne(Invert{}, Rgbaf{0.2f, 0.2f, 0.2f, 0.0f});
    PE_CHECK_NEAR(out.r, 0.2f);  // transparent -> untouched
}

PE_TEST(compositor_adjustment_inverts_backdrop) {
    PixelBuffer img = renderAdjusted(std::make_unique<Invert>());
    PE_CHECK(near8(img.at(0, 0), kCyan));  // red inverted -> cyan
}

PE_TEST(compositor_adjustment_opacity_fades) {
    PixelBuffer img = renderAdjusted(std::make_unique<Invert>(), nullptr, 0.5f);
    PE_CHECK(near8(img.at(0, 0), Rgba8{128, 128, 128, 255}));  // halfway red->cyan
}

PE_TEST(compositor_adjustment_masked) {
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{4, 0, 4, kH}, MaskBuffer::kClear);  // hide right half
    PixelBuffer img = renderAdjusted(std::make_unique<Invert>(), std::move(mask));
    PE_CHECK(near8(img.at(0, 0), kCyan));  // left: adjusted
    PE_CHECK(near8(img.at(6, 0), kRed));   // right: masked out -> original
}

PE_TEST(adjustment_non_destructive_undo) {
    auto doc = Document::createBlank(Size{kW, kH});
    doc->history().push(std::make_unique<AddLayerCommand>(
        std::make_unique<SolidColorLayer>(kRed, kCanvas), doc->topLevelCount()));
    PE_CHECK(near8(doc->compositeImage().at(0, 0), kRed));

    doc->history().push(std::make_unique<AddLayerCommand>(
        std::make_unique<AdjustmentLayer>(std::make_unique<Invert>(), "Invert"),
        doc->topLevelCount()));
    PE_CHECK(near8(doc->compositeImage().at(0, 0), kCyan));

    doc->history().undo();                                  // remove the adjustment
    PE_CHECK(near8(doc->compositeImage().at(0, 0), kRed));  // original restored, non-destructively
}

PE_TEST(edit_adjustment_command_roundtrip) {
    auto doc = Document::createBlank(Size{kW, kH});
    doc->history().push(std::make_unique<AddLayerCommand>(
        std::make_unique<SolidColorLayer>(kRed, kCanvas), doc->topLevelCount()));
    auto adjLayer =
        std::make_unique<AdjustmentLayer>(std::make_unique<BrightnessContrast>(0.0f, 0.0f), "Adj");
    const LayerId adjId = adjLayer->id();
    doc->history().push(
        std::make_unique<AddLayerCommand>(std::move(adjLayer), doc->topLevelCount()));
    PE_CHECK(near8(doc->compositeImage().at(0, 0), kRed));  // identity adjustment

    doc->history().push(std::make_unique<EditAdjustmentCommand>(adjId, std::make_unique<Invert>()));
    PE_CHECK(near8(doc->compositeImage().at(0, 0), kCyan));  // now inverts

    doc->history().undo();  // back to the identity Brightness/Contrast
    PE_CHECK(near8(doc->compositeImage().at(0, 0), kRed));
}
