#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"  // kMaxCanvasDimension
#include "pe/core/Mask.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>
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

// ---------------------------------------------------------------------------
// MaskBuffer::translate. The crop command relies on this to keep a layer mask
// aligned with the pixels it masks when the canvas origin moves.
// ---------------------------------------------------------------------------

namespace {
constexpr int64_t kAmpleBudget = 16'000'000;

// A recognisable pattern: a hidden square with one distinct interior value, so a
// translation that loses or resamples data is visible rather than plausible.
MaskBuffer patternedMask() {
    MaskBuffer m;
    m.fillRect(Rect{10, 20, 30, 40}, MaskBuffer::kClear);
    m.setValue(15, 25, 77);
    return m;
}
}  // namespace

PE_TEST(maskbuffer_translate_moves_coverage_by_the_delta) {
    MaskBuffer m = patternedMask();
    PE_CHECK(m.translate(7, -3, kAmpleBudget));

    // Moved to the new position...
    PE_CHECK_EQ(static_cast<int>(m.value(10 + 7, 20 - 3)), 0);
    PE_CHECK_EQ(static_cast<int>(m.value(15 + 7, 25 - 3)), 77);
    PE_CHECK_EQ(static_cast<int>(m.value(39 + 7, 59 - 3)), 0);
    // ...and the vacated area reveals again (absent reads kOpaque).
    PE_CHECK_EQ(static_cast<int>(m.value(10, 20)), 255);
    PE_CHECK_EQ(static_cast<int>(m.value(15, 25)), 255);
}

PE_TEST(maskbuffer_translate_is_exactly_invertible) {
    MaskBuffer m = patternedMask();
    const std::size_t tilesBefore = m.tileCount();
    const Rect boundsBefore = m.contentBounds();

    PE_CHECK(m.translate(13, -29, kAmpleBudget));
    PE_CHECK(m.translate(-13, 29, kAmpleBudget));

    PE_CHECK_EQ(m.tileCount(), tilesBefore);
    PE_CHECK(m.contentBounds() == boundsBefore);
    for (int y = 18; y < 62; ++y) {
        for (int x = 8; x < 42; ++x) {
            const uint8_t expected = (x == 15 && y == 25) ? 77
                                     : (x >= 10 && x < 40 && y >= 20 && y < 60)
                                         ? MaskBuffer::kClear
                                         : MaskBuffer::kOpaque;
            PE_CHECK_EQ(static_cast<int>(m.value(x, y)), static_cast<int>(expected));
        }
    }
}

PE_TEST(maskbuffer_translate_tile_aligned_is_a_pure_rekey) {
    // A whole-tile shift needs no per-pixel work, so it must succeed even with a
    // budget far too small for the general path.
    MaskBuffer m = patternedMask();
    const std::size_t tilesBefore = m.tileCount();
    PE_CHECK(m.translate(kTileSize * 2, -kTileSize, /*maxPixels=*/1));
    PE_CHECK_EQ(m.tileCount(), tilesBefore);
    PE_CHECK_EQ(static_cast<int>(m.value(15 + kTileSize * 2, 25 - kTileSize)), 77);
    PE_CHECK_EQ(static_cast<int>(m.value(15, 25)), 255);
}

PE_TEST(maskbuffer_translate_over_budget_refuses_and_leaves_the_buffer_untouched) {
    MaskBuffer m = patternedMask();
    const std::size_t tilesBefore = m.tileCount();
    const Rect boundsBefore = m.contentBounds();

    PE_CHECK(!m.translate(1, 1, /*maxPixels=*/4));

    // Refusal must be all-or-nothing: a half-translated mask is worse than none.
    PE_CHECK_EQ(m.tileCount(), tilesBefore);
    PE_CHECK(m.contentBounds() == boundsBefore);
    PE_CHECK_EQ(static_cast<int>(m.value(15, 25)), 77);
}

PE_TEST(maskbuffer_translate_out_of_range_refuses) {
    // The bound is inclusive: a destination edge landing exactly on
    // +/-kMaxCanvasDimension is representable and allowed; one past it is not.
    MaskBuffer m = patternedMask();
    const Rect boundsBefore = m.contentBounds();

    // contentBounds is tile-granular, so the right edge is what crosses first here.
    PE_CHECK(!m.translate(kMaxCanvasDimension, 0, kAmpleBudget));
    PE_CHECK(!m.translate(0, -(kMaxCanvasDimension + 1), kAmpleBudget));
    PE_CHECK(m.contentBounds() == boundsBefore);

    // Exactly on the bound succeeds, which is what makes the rejections above a
    // boundary test rather than a vague "large numbers fail". Assert on a moved
    // pixel rather than on contentBounds(), which is tile-granular and so snaps
    // outward to the enclosing tile.
    MaskBuffer edge = patternedMask();
    const int toEdge = -(kMaxCanvasDimension + edge.contentBounds().top());
    PE_CHECK(edge.translate(0, toEdge, kAmpleBudget));
    PE_CHECK_EQ(static_cast<int>(edge.value(15, 25 + toEdge)), 77);
    PE_CHECK_EQ(static_cast<int>(edge.value(15, 25)), 255);
}

PE_TEST(maskbuffer_translate_zero_and_empty_are_no_ops) {
    MaskBuffer empty;
    PE_CHECK(empty.translate(5, 5, kAmpleBudget));
    PE_CHECK(empty.empty());

    MaskBuffer m = patternedMask();
    const std::size_t tilesBefore = m.tileCount();
    PE_CHECK(m.translate(0, 0, /*maxPixels=*/0));
    PE_CHECK_EQ(m.tileCount(), tilesBefore);
}
