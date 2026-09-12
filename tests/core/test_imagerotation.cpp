// Image Rotation: OrientDocumentCommand reorients the whole document -- canvas, every pixel layer
// (groups included), masks, text rasters, fill bounds and the selection -- exactly (a permutation,
// lossless) as one undoable step. The orientation geometry is pinned in test_orient; this drives
// the command that applies it to a real document, its refusal, and undo.

#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/Orient.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe/core/TextLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <memory>

using namespace pe;

namespace {

PixelLayer* base(Document& doc) {
    return static_cast<PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

OrientDocumentCommand* orient(Document& doc, Orient op) {
    auto cmd = std::make_unique<OrientDocumentCommand>(op);
    auto* raw = cmd.get();
    doc.history().push(std::move(cmd));
    return raw;
}

}  // namespace

PE_TEST(orientcmd_rotate90cw_swaps_the_canvas_and_moves_pixels) {
    auto doc = Document::createBlank(Size{4, 3});
    base(*doc)->tiles().setPixel(0, 0, Rgba8{200, 50, 50, 255});  // top-left mark

    auto* cmd = orient(*doc, Orient::Rotate90CW);
    PE_CHECK(cmd->reoriented());
    PE_CHECK(doc->canvasSize() == (Size{3, 4}));  // sides swapped
    // (0,0) maps to (H-1, 0) = (2,0) on the new 3x4 canvas.
    PE_CHECK_EQ(base(*doc)->tiles().pixel(2, 0), (Rgba8{200, 50, 50, 255}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(0, 0).a, static_cast<uint8_t>(0));  // vacated

    doc->history().undo();
    PE_CHECK(doc->canvasSize() == (Size{4, 3}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(0, 0), (Rgba8{200, 50, 50, 255}));  // restored
    PE_CHECK_EQ(base(*doc)->tiles().pixel(2, 0).a, static_cast<uint8_t>(0));
}

PE_TEST(orientcmd_flip_horizontal_mirrors_and_keeps_the_canvas) {
    auto doc = Document::createBlank(Size{4, 3});
    base(*doc)->tiles().setPixel(0, 1, Rgba8{10, 200, 30, 255});

    orient(*doc, Orient::FlipHorizontal);
    PE_CHECK(doc->canvasSize() == (Size{4, 3}));                              // unchanged
    PE_CHECK_EQ(base(*doc)->tiles().pixel(3, 1), (Rgba8{10, 200, 30, 255}));  // W-1-0 = 3
    PE_CHECK_EQ(base(*doc)->tiles().pixel(0, 1).a, static_cast<uint8_t>(0));

    doc->history().undo();
    PE_CHECK_EQ(base(*doc)->tiles().pixel(0, 1), (Rgba8{10, 200, 30, 255}));
}

PE_TEST(orientcmd_rotate180_sends_a_corner_to_the_opposite_corner) {
    auto doc = Document::createBlank(Size{4, 3});
    base(*doc)->tiles().setPixel(0, 0, Rgba8{40, 60, 220, 255});
    orient(*doc, Orient::Rotate180);
    PE_CHECK(doc->canvasSize() == (Size{4, 3}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(3, 2), (Rgba8{40, 60, 220, 255}));  // (W-1,H-1)
    doc->history().undo();
    PE_CHECK_EQ(base(*doc)->tiles().pixel(0, 0), (Rgba8{40, 60, 220, 255}));
}

PE_TEST(orientcmd_rotates_a_layer_mask_with_the_pixels) {
    auto doc = Document::createBlank(Size{64, 48});
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{0, 0, 16, 48}, MaskBuffer::kClear);  // hide the left strip
    base(*doc)->setMask(std::move(mask));

    orient(*doc, Orient::FlipHorizontal);  // the hidden strip must move to the right edge
    const MaskBuffer& mb = base(*doc)->mask()->buffer();
    PE_CHECK_EQ(mb.value(60, 20), MaskBuffer::kClear);  // was x<16, now mirrored to x>=48
    PE_CHECK_EQ(mb.value(5, 20), MaskBuffer::kOpaque);  // left now revealed

    doc->history().undo();
    PE_CHECK_EQ(base(*doc)->mask()->buffer().value(5, 20), MaskBuffer::kClear);  // restored
}

PE_TEST(orientcmd_rotates_a_text_layer_raster_and_origin) {
    auto doc = Document::createBlank(Size{4, 3});
    PixelBuffer raster(2, 1, Rgba8{0, 0, 0, 255});
    raster.set(0, 0, Rgba8{200, 0, 0, 255});  // left red
    raster.set(1, 0, Rgba8{0, 0, 200, 255});  // right blue
    auto text = std::make_unique<TextLayer>(TextModel{}, std::move(raster), Point{0, 0});
    const LayerId id = text->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(text));

    orient(*doc, Orient::Rotate90CW);
    auto* t = static_cast<TextLayer*>(doc->findLayer(id));
    PE_REQUIRE(t != nullptr);
    PE_CHECK_EQ(t->raster().width(), 1);  // 2x1 rotated -> 1x2
    PE_CHECK_EQ(t->raster().height(), 2);
    PE_CHECK(t->rasterOrigin() == (Point{2, 0}));  // box {0,0,2,1} -> {2,0,1,2}

    doc->history().undo();
    t = static_cast<TextLayer*>(doc->findLayer(id));
    PE_CHECK_EQ(t->raster().width(), 2);
    PE_CHECK_EQ(t->raster().height(), 1);
    PE_CHECK(t->rasterOrigin() == (Point{0, 0}));
}

PE_TEST(orientcmd_rotates_a_fill_layer_bounds) {
    auto doc = Document::createBlank(Size{4, 3});
    auto fill = std::make_unique<SolidColorLayer>(Rgba8{0, 128, 255, 255}, Rect{0, 0, 2, 1});
    const LayerId id = fill->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(fill));

    orient(*doc, Orient::Rotate90CW);
    PE_CHECK(static_cast<SolidColorLayer*>(doc->findLayer(id))->bounds() == (Rect{2, 0, 1, 2}));
    doc->history().undo();
    PE_CHECK(static_cast<SolidColorLayer*>(doc->findLayer(id))->bounds() == (Rect{0, 0, 2, 1}));
}

PE_TEST(orientcmd_rotates_the_selection) {
    auto doc = Document::createBlank(Size{100, 60});
    doc->editableSelection().selectRect(Rect{0, 0, 20, 10});
    doc->touchSelection();
    PE_REQUIRE(doc->selection().active());

    orient(*doc, Orient::Rotate90CW);  // canvas -> 60x100; {0,0,20,10} -> a rotated box
    PE_REQUIRE(doc->selection().active());
    // {0,0,20,10} corners (0,0)->(59,0), (19,9)->(50,19); bbox {50,0,10,20}.
    PE_CHECK(doc->selection().tightBounds() == (Rect{50, 0, 10, 20}));

    doc->history().undo();
    PE_CHECK(doc->selection().tightBounds() == (Rect{0, 0, 20, 10}));
}

PE_TEST(orientcmd_rotates_content_inside_groups) {
    auto doc = Document::createBlank(Size{4, 3});
    auto group = std::make_unique<GroupLayer>("Grp");
    auto child = std::make_unique<PixelLayer>("Inner");
    child->tiles().setPixel(0, 0, Rgba8{10, 200, 30, 255});
    const LayerId childId = child->id();
    group->addChild(std::move(child));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    orient(*doc, Orient::Rotate90CW);
    PE_CHECK(doc->canvasSize() == (Size{3, 4}));
    PE_CHECK_EQ(static_cast<PixelLayer*>(doc->findLayer(childId))->tiles().pixel(2, 0),
                (Rgba8{10, 200, 30, 255}));
    doc->history().undo();
    PE_CHECK_EQ(static_cast<PixelLayer*>(doc->findLayer(childId))->tiles().pixel(0, 0),
                (Rgba8{10, 200, 30, 255}));
}

PE_TEST(orientcmd_undo_then_redo_round_trips) {
    auto doc = Document::createBlank(Size{4, 3});
    base(*doc)->tiles().setPixel(1, 2, Rgba8{123, 45, 67, 255});
    orient(*doc, Orient::Rotate90CCW);
    const Rgba8 after = base(*doc)->tiles().pixel(2, 2);  // wherever it landed
    PE_CHECK(doc->canvasSize() == (Size{3, 4}));
    doc->history().undo();
    PE_CHECK(doc->canvasSize() == (Size{4, 3}));
    doc->history().redo();
    PE_CHECK(doc->canvasSize() == (Size{3, 4}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(2, 2), after);
}

PE_TEST(orientcmd_retained_bytes_counts_the_snapshots) {
    auto doc = Document::createBlank(Size{64, 48});
    base(*doc)->tiles().fillRect(Rect{0, 0, 64, 48}, Rgba8{80, 80, 80, 255});
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{0, 0, 32, 48}, MaskBuffer::kClear);
    base(*doc)->setMask(std::move(mask));
    doc->editableSelection().selectRect(Rect{4, 4, 20, 20});
    doc->touchSelection();

    auto* cmd = orient(*doc, Orient::Rotate90CW);
    PE_CHECK(cmd->reoriented());
    PE_CHECK(cmd->retainedBytes() > 0);
}

PE_TEST(orientcmd_refuses_content_too_large_to_reorient) {
    // A layer whose content spans a huge box cannot be reoriented within the move budget: the
    // command refuses as a whole (no-op) rather than half-turning the document.
    auto doc = Document::createBlank(Size{40000, 40000});
    base(*doc)->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
    base(*doc)->tiles().setPixel(39999, 39999, Rgba8{4, 5, 6, 255});  // content bbox ~40000^2
    PE_CHECK(orientBlocker(*doc, Orient::Rotate90CW) == OrientBlock::ContentTooLarge);

    auto* cmd = orient(*doc, Orient::Rotate90CW);
    PE_CHECK(!cmd->reoriented());
    PE_CHECK(doc->canvasSize() == (Size{40000, 40000}));  // untouched
}
