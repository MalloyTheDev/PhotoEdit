#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <cstdint>
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

PE_TEST(maskbrush_emits_maskpixels_change) {
    // A mask stroke reports Kind::MaskPixels (not LayerProps) so the renderer recomposites the
    // region while the Layers panel refreshes only the affected row instead of a full tree rebuild.
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId base = doc->activeLayer();
    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, /*targetGray=*/0.0f);
    PE_CHECK(cmd != nullptr);
    const DocumentChange ch = cmd->execute(*doc);
    PE_CHECK(ch.kind == DocumentChange::Kind::MaskPixels);
    PE_CHECK_EQ(ch.layer, base);
    PE_CHECK(!ch.dirtyRegion.isEmpty());  // the painted region, so the renderer invalidates it
    const DocumentChange chu = cmd->undo(*doc);
    PE_CHECK(chu.kind == DocumentChange::Kind::MaskPixels);  // undo reports it too
}

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

PE_TEST(maskbrush_inverted_mask_honors_convention) {
    // On an INVERTED mask "white reveals" must still hold: the engine writes the complement so
    // Mask::evaluate() (which flips inverted bytes) reveals where the user paints white. Without
    // the inversion handling, white (target 255 == the empty default) would be a no-op -> nullptr.
    auto doc = redDocWithMask(/*revealAll=*/true);  // empty buffer
    const LayerId base = doc->activeLayer();
    auto* mask = static_cast<PixelLayer*>(doc->findLayer(base))->mask();
    mask->setInverted(true);  // empty buffer (255) now evaluates to 0 -> fully hidden
    PE_CHECK_EQ(compositeAlpha(*doc, 32, 32), 0);  // inverted reveal-all hides everything

    std::vector<StrokePoint> pts = {{{32, 32}, 1.0f}};
    auto cmd = maskPaintStroke(*doc, base, hardBrush(16, 1.0f), pts, /*targetGray=*/1.0f);  // white
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(compositeAlpha(*doc, 32, 32) > 200);  // white revealed it (convention holds)
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

namespace {

// Every mask byte over a region, for byte-for-byte comparison of two runs.
std::vector<std::uint8_t> maskBytes(const Document& doc, LayerId id, Rect region) {
    const Layer* layer = doc.findLayer(id);
    const MaskBuffer& buf = layer->mask()->buffer();
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(region.width) * static_cast<std::size_t>(region.height));
    for (int y = region.top(); y < region.bottom(); ++y) {
        for (int x = region.left(); x < region.right(); ++x) out.push_back(buf.value(x, y));
    }
    return out;
}

std::vector<StrokePoint> zigzag() {
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 24; ++i) {
        const auto t = static_cast<float>(i);
        pts.push_back(StrokePoint{Vec2{6.0f + t * 2.1f, 8.0f + (i % 2 == 0 ? t : t * 0.4f)},
                                  0.4f + 0.02f * t});
    }
    return pts;
}

// The incremental mask stroke, fed in `chunk`-sized steps, must leave the mask identical to
// the batched maskPaintStroke, and commit a command that undoes and redoes byte-exactly.
void checkMaskParity(const BrushSettings& brush, float targetGray, bool inverted,
                     const Selection* sel, int chunk) {
    const Rect all{0, 0, 64, 64};
    const std::vector<StrokePoint> pts = zigzag();

    // Batched reference.
    auto da = redDocWithMask(/*revealAll=*/true);
    const LayerId la = da->activeLayer();
    da->findLayer(la)->mask()->setInverted(inverted);
    da->findLayer(la)->mask()->buffer().fillRect(Rect{10, 10, 20, 20}, 90);  // pre-existing content
    const std::vector<std::uint8_t> initial = maskBytes(*da, la, all);
    auto cmdA = maskPaintStroke(*da, la, brush, pts, targetGray, sel);
    PE_CHECK(cmdA != nullptr);
    cmdA->execute(*da);
    const std::vector<std::uint8_t> reference = maskBytes(*da, la, all);
    PE_CHECK(reference != initial);  // the stroke must actually do something

    // Incremental, fed in chunks.
    auto db = redDocWithMask(/*revealAll=*/true);
    const LayerId lb = db->activeLayer();
    db->findLayer(lb)->mask()->setInverted(inverted);
    db->findLayer(lb)->mask()->buffer().fillRect(Rect{10, 10, 20, 20}, 90);
    auto live = beginMaskPaintStroke(*db, lb, brush, targetGray, sel);
    PE_CHECK(live != nullptr);
    std::vector<StrokePoint> acc;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        acc.push_back(pts[i]);
        if (static_cast<int>(acc.size()) % chunk == 0 || i + 1 == pts.size()) {
            (void)live->extend(acc);
        }
    }
    PE_CHECK(!live->atBudget());
    auto cmdB = live->finish();
    PE_CHECK(cmdB != nullptr);

    PE_CHECK(maskBytes(*db, lb, all) == reference);  // incremental == batched, byte for byte

    // The committed command is a byte-exact single undo step: the mask already holds the
    // final bytes, so pushing re-applies a no-op, undo restores the original, redo repeats.
    db->history().push(std::move(cmdB));
    PE_CHECK(maskBytes(*db, lb, all) == reference);
    db->history().undo();
    PE_CHECK(maskBytes(*db, lb, all) == initial);
    db->history().redo();
    PE_CHECK(maskBytes(*db, lb, all) == reference);
}

}  // namespace

PE_TEST(livemask_matches_batched_one_sample_at_a_time) {
    // One point per extend() is the case that would expose any dependence on seeing the
    // whole path at once.
    checkMaskParity(hardBrush(9.0f, 1.0f), 0.0f, false, nullptr, 1);
}

PE_TEST(livemask_matches_batched_in_chunks) {
    for (const int chunk : {2, 3, 7}) {
        checkMaskParity(hardBrush(9.0f, 1.0f), 0.0f, false, nullptr, chunk);
    }
}

PE_TEST(livemask_matches_batched_at_partial_opacity) {
    // Partial opacity and a mid-grey target exercise the lround, which is where a
    // re-derivation from a snapshot would differ from a single pass if it were not exact.
    checkMaskParity(hardBrush(11.0f, 0.45f), 0.6f, false, nullptr, 1);
    checkMaskParity(hardBrush(11.0f, 0.45f), 0.6f, false, nullptr, 4);
}

PE_TEST(livemask_matches_batched_on_an_inverted_mask) {
    checkMaskParity(hardBrush(9.0f, 1.0f), 1.0f, true, nullptr, 2);
}

PE_TEST(livemask_matches_batched_when_gated_by_a_selection) {
    Selection sel;
    sel.selectRect(Rect{12, 0, 26, 64});
    checkMaskParity(hardBrush(9.0f, 1.0f), 0.0f, false, &sel, 1);
    checkMaskParity(hardBrush(9.0f, 1.0f), 0.0f, false, &sel, 5);
}

PE_TEST(livemask_matches_batched_with_a_feathered_selection) {
    // Partial selection coverage multiplies into the stroke coverage, so the gate is no
    // longer a switch and any drift in the arithmetic shows up as a different byte.
    Selection sel;
    sel.selectRect(Rect{12, 0, 26, 64});
    sel.feather(4.0f, Rect{0, 0, 64, 64});
    checkMaskParity(hardBrush(11.0f, 0.8f), 0.2f, false, &sel, 3);
}

PE_TEST(livemask_cancel_restores_the_mask_exactly) {
    auto doc = redDocWithMask(/*revealAll=*/true);
    const LayerId id = doc->activeLayer();
    doc->findLayer(id)->mask()->buffer().fillRect(Rect{10, 10, 20, 20}, 90);
    const Rect all{0, 0, 64, 64};
    const std::vector<std::uint8_t> initial = maskBytes(*doc, id, all);
    const std::size_t tilesBefore = doc->findLayer(id)->mask()->buffer().tileCount();

    auto live = beginMaskPaintStroke(*doc, id, hardBrush(9.0f, 1.0f), 0.0f, nullptr);
    PE_CHECK(live != nullptr);
    (void)live->extend(zigzag());
    PE_CHECK(maskBytes(*doc, id, all) != initial);  // it really painted
    live->cancel();

    PE_CHECK(maskBytes(*doc, id, all) == initial);
    // And no redundant fully-revealing tile is left behind, which would grow
    // contentBounds and defeat the compositor's empty-mask fast path.
    PE_CHECK_EQ(doc->findLayer(id)->mask()->buffer().tileCount(), tilesBefore);
}

PE_TEST(livemask_stroke_past_the_budget_freezes_instead_of_losing_the_stroke) {
    // The command stores a dense array over the stroke's bounding box, so the box is
    // capped. The live path must stop accepting samples rather than paint pixels it cannot
    // commit: finish() returning nullptr would leave the mask mutated with no undo entry,
    // which is worse than the batched refusal it replaces.
    auto doc = Document::createBlank(Size{kMaxCanvasDimension, kMaxCanvasDimension});
    PE_CHECK(doc != nullptr);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->setMask(std::make_unique<Mask>(Mask::Kind::Layer));
    const LayerId id = doc->activeLayer();

    auto live = beginMaskPaintStroke(*doc, id, hardBrush(9.0f, 1.0f), 0.0f, nullptr);
    PE_CHECK(live != nullptr);
    std::vector<StrokePoint> acc{StrokePoint{Vec2{10.0f, 10.0f}, 1.0f}};
    (void)live->extend(acc);
    PE_CHECK(!live->atBudget());

    // A jump far enough that the bounding box passes 16 MP.
    acc.push_back(StrokePoint{Vec2{9000.0f, 9000.0f}, 1.0f});
    (void)live->extend(acc);
    PE_CHECK(live->atBudget());

    // What it painted before freezing still commits.
    auto cmd = live->finish();
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));
    PE_CHECK(doc->findLayer(id)->mask()->buffer().value(10, 10) < MaskBuffer::kOpaque);
}
