#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <memory>
#include <vector>

using namespace pe;

namespace {
BrushSettings hardBrush(float diameter, float opacity) {
    BrushSettings b;
    b.diameter = diameter;
    b.hardness = 1.0f;
    b.opacity = opacity;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    return b;
}
// A 64x64 doc with an opaque-red base layer; `mask` decides the attached mask (none if false).
std::unique_ptr<Document> redDocWithMask(bool revealAll) {
    auto doc = Document::createBlank(Size{64, 64});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{255, 0, 0, 255});
    auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
    if (!revealAll) mask->buffer().fillRect(Rect{0, 0, 64, 64}, MaskBuffer::kClear);  // hide all
    pl->setMask(std::move(mask));
    return doc;
}
int compositeAlpha(Document& doc, int x, int y) {
    const PixelBuffer img = doc.compositeImage();
    return img.isEmpty() ? -1 : static_cast<int>(img.at(x, y).a);
}
}  // namespace

PE_TEST(maskbrush_black_hides) {
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, /*targetGray=*/0.0f);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(compositeAlpha(*doc, 32, 32) < 40);   // painted black -> hidden at center
    PE_CHECK_EQ(compositeAlpha(*doc, 2, 2), 255);  // untouched -> still revealed

    doc->history().undo();
    PE_CHECK_EQ(compositeAlpha(*doc, 32, 32), 255);  // undo restores reveal
}

PE_TEST(maskbrush_white_reveals_on_hidden_mask) {
    auto doc = redDocWithMask(/*revealAll=*/false);  // start fully hidden
    const LayerId base = doc->activeLayer();
    PE_CHECK_EQ(compositeAlpha(*doc, 32, 32), 0);  // hidden everywhere
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, /*targetGray=*/1.0f);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(compositeAlpha(*doc, 32, 32) > 200);  // painted white -> revealed at center
    PE_CHECK_EQ(compositeAlpha(*doc, 2, 2), 0);    // untouched -> still hidden
}

PE_TEST(maskbrush_undo_is_tile_exact) {
    // Painting into a reveal-all (empty) mask then undoing must restore the buffer's TILE state
    // exactly, not just its evaluated values: an all-kOpaque tile left allocated would grow
    // contentBounds() / the serialized mask and defeat the compositor's empty-mask fast path.
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId base = doc->activeLayer();
    auto* mask = static_cast<PixelLayer*>(doc->findLayer(base))->mask();
    PE_CHECK(mask->buffer().empty());  // reveal-all starts with no tiles
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, /*targetGray=*/0.0f);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(!mask->buffer().empty());  // the stroke allocated tiles
    doc->history().undo();
    PE_CHECK(mask->buffer().empty());                    // undo dropped them back to none
    PE_CHECK(mask->buffer().contentBounds().isEmpty());  // and contentBounds is back to empty
}

PE_TEST(maskbrush_partial_opacity_rounds) {
    // opacity 0.5, paint black (target 0) on a reveal-all (255) mask: each fully-covered byte
    // blends to 255 + (0 - 255) * 0.5 = 127.5 -> lround -> 128, i.e. ~50% coverage in the
    // composite.
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(16, 0.5f), pts, /*targetGray=*/0.0f);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    const int a = compositeAlpha(*doc, 32, 32);
    PE_CHECK(a >= 124 && a <= 132);  // ~128 (half-revealed)
}

PE_TEST(maskbrush_no_mask_is_null) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    static_cast<PixelLayer*>(doc->findLayer(base))
        ->tiles()
        .fillRect(Rect{0, 0, 64, 64}, Rgba8{255, 0, 0, 255});
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    // No mask attached: nothing to paint.
    PE_CHECK(maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, 0.0f) == nullptr);
}

PE_TEST(maskbrush_no_change_is_null) {
    // Painting white (reveal) on an already-fully-revealed mask changes nothing -> no command.
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    PE_CHECK(maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, /*targetGray=*/1.0f) == nullptr);
}

PE_TEST(maskbrush_honors_selection) {
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId base = doc->activeLayer();
    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 64});  // left half only
    // A wide black stroke across the middle; only the selected (left) half should be masked out.
    std::vector<StrokePoint> pts = {{{8, 32}, 1.0f}, {{56, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(20, 1.0f), pts, /*targetGray=*/0.0f, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(compositeAlpha(*doc, 8, 32) < 40);      // inside the selection -> masked out
    PE_CHECK_EQ(compositeAlpha(*doc, 56, 32), 255);  // outside the selection -> mask untouched
}
