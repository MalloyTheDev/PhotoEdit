#include "pe/core/Compositor.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/Selection.hpp"
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

bool near8(Rgba8 a, Rgba8 b, int tol = 1) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol && d(a.a, b.a) <= tol;
}

// Composite a single red layer carrying `mask` over the canvas.
PixelBuffer renderMasked(std::unique_ptr<Mask> mask) {
    auto layer = std::make_unique<SolidColorLayer>(kRed, kCanvas);
    layer->setMask(std::move(mask));
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(std::move(layer));
    return compositeToImage(stack, kCanvas);
}
}  // namespace

PE_TEST(maskbuffer_absent_is_opaque) {
    MaskBuffer b;
    PE_CHECK(b.empty());
    PE_CHECK_EQ(b.value(0, 0), MaskBuffer::kOpaque);  // absent -> revealing
    PE_CHECK_EQ(b.value(-50, 99), MaskBuffer::kOpaque);
    b.setValue(3, 3, MaskBuffer::kClear);
    PE_CHECK_EQ(b.value(3, 3), MaskBuffer::kClear);
    PE_CHECK_EQ(b.value(4, 3), MaskBuffer::kOpaque);  // neighbor still revealing
}

PE_TEST(maskbuffer_setvalue_opaque_does_not_allocate) {
    MaskBuffer b;
    b.setValue(5, 5, MaskBuffer::kOpaque);  // default value -> no tile
    PE_CHECK(b.empty());
}

PE_TEST(mask_evaluate_density_and_invert) {
    Mask m;
    m.buffer().setValue(0, 0, 128);
    PE_CHECK_NEAR(m.evaluate(0, 0), 128.0f / 255.0f);
    m.setDensity(0.5f);
    PE_CHECK_NEAR(m.evaluate(0, 0), (128.0f / 255.0f) * 0.5f);
    m.setDensity(1.0f);
    m.setInverted(true);
    PE_CHECK_NEAR(m.evaluate(0, 0), 1.0f - 128.0f / 255.0f);
}

PE_TEST(compositor_layer_mask_hides_region) {
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{0, 0, 4, kH}, MaskBuffer::kClear);  // hide left half
    PixelBuffer img = renderMasked(std::move(mask));
    PE_CHECK_EQ(img.at(0, 0).a, 0);       // hidden
    PE_CHECK(near8(img.at(6, 0), kRed));  // revealed
}

PE_TEST(compositor_white_mask_is_noop) {
    auto mask = std::make_unique<Mask>();  // empty buffer == fully revealing
    PixelBuffer img = renderMasked(std::move(mask));
    PE_CHECK(near8(img.at(0, 0), kRed));
    PE_CHECK(near8(img.at(7, 7), kRed));
}

PE_TEST(compositor_inverted_empty_mask_hides_all) {
    auto mask = std::make_unique<Mask>();
    mask->setInverted(true);  // empty buffer (255) inverted -> 0 everywhere
    PixelBuffer img = renderMasked(std::move(mask));
    PE_CHECK_EQ(img.at(0, 0).a, 0);
    PE_CHECK_EQ(img.at(7, 7).a, 0);
}

PE_TEST(compositor_density_half_mask) {
    auto mask = std::make_unique<Mask>();
    mask->setDensity(0.5f);  // empty buffer (reveal) at 50% density
    PixelBuffer img = renderMasked(std::move(mask));
    PE_CHECK(near8(img.at(0, 0), Rgba8{255, 0, 0, 128}));
}

PE_TEST(compositor_disabled_mask_ignored) {
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(kCanvas, MaskBuffer::kClear);  // would hide everything
    mask->setEnabled(false);                               // ...but disabled
    PixelBuffer img = renderMasked(std::move(mask));
    PE_CHECK(near8(img.at(0, 0), kRed));  // mask ignored
}

PE_TEST(mask_from_selection_reveals_selected) {
    Selection sel;
    sel.selectRect(Rect{0, 0, 4, kH});  // left half selected
    auto mask = std::make_unique<Mask>(maskFromSelection(sel, kCanvas));
    PixelBuffer img = renderMasked(std::move(mask));
    PE_CHECK(near8(img.at(0, 0), kRed));  // selected -> revealed
    PE_CHECK_EQ(img.at(6, 0).a, 0);       // unselected -> hidden
}

PE_TEST(layer_clone_deep_copies_mask) {
    auto layer = std::make_unique<SolidColorLayer>(kRed, kCanvas);
    auto mask = std::make_unique<Mask>();
    mask->buffer().setValue(1, 1, MaskBuffer::kClear);
    layer->setMask(std::move(mask));

    auto clone = layer->clone();
    PE_CHECK(clone->mask() != nullptr);
    PE_CHECK(clone->mask() != layer->mask());  // independent instance
    // Mutating the clone's mask doesn't touch the original.
    clone->mask()->buffer().setValue(2, 2, MaskBuffer::kClear);
    PE_CHECK_EQ(layer->mask()->buffer().value(2, 2), MaskBuffer::kOpaque);
}
