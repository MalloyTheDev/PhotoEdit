#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <memory>

using namespace pe;

namespace {
PixelLayer* base(Document& doc) {
    return static_cast<PixelLayer*>(doc.findLayer(doc.activeLayer()));
}
}  // namespace

PE_TEST(crop_resizes_and_shifts_content_undoable) {
    auto doc = Document::createBlank(Size{64, 64});
    base(*doc)->tiles().setPixel(20, 18, Rgba8{200, 50, 50, 255});  // a distinct mark

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));
    PE_CHECK_EQ(doc->canvasSize().width, 30);
    PE_CHECK_EQ(doc->canvasSize().height, 30);
    // Content shifts by -(10,8): the mark at (20,18) moves to (10,10) inside the new canvas.
    PE_CHECK_EQ(base(*doc)->tiles().pixel(10, 10), (Rgba8{200, 50, 50, 255}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(20, 18).a, static_cast<uint8_t>(0));  // vacated

    doc->history().undo();
    PE_CHECK_EQ(doc->canvasSize().width, 64);
    PE_CHECK_EQ(doc->canvasSize().height, 64);
    PE_CHECK_EQ(base(*doc)->tiles().pixel(20, 18), (Rgba8{200, 50, 50, 255}));  // restored
    PE_CHECK_EQ(base(*doc)->tiles().pixel(10, 10).a, static_cast<uint8_t>(0));
}

PE_TEST(crop_from_origin_only_resizes) {
    auto doc = Document::createBlank(Size{64, 64});
    base(*doc)->tiles().setPixel(5, 5, Rgba8{1, 2, 3, 255});

    doc->history().push(std::make_unique<CropCommand>(Rect{0, 0, 20, 20}));  // no content shift
    PE_CHECK_EQ(doc->canvasSize().width, 20);
    PE_CHECK_EQ(doc->canvasSize().height, 20);
    PE_CHECK_EQ(base(*doc)->tiles().pixel(5, 5), (Rgba8{1, 2, 3, 255}));  // content unmoved

    doc->history().undo();
    PE_CHECK_EQ(doc->canvasSize().width, 64);
}

PE_TEST(crop_clamps_rect_to_canvas) {
    auto doc = Document::createBlank(Size{32, 32});
    // A rect partly off-canvas is clamped to the canvas intersection: (20,20)+12x12.
    doc->history().push(std::make_unique<CropCommand>(Rect{20, 20, 100, 100}));
    PE_CHECK_EQ(doc->canvasSize().width, 12);
    PE_CHECK_EQ(doc->canvasSize().height, 12);

    doc->history().undo();
    PE_CHECK_EQ(doc->canvasSize().width, 32);
    PE_CHECK_EQ(doc->canvasSize().height, 32);
}

PE_TEST(crop_fully_offcanvas_is_noop) {
    auto doc = Document::createBlank(Size{32, 32});
    doc->history().push(std::make_unique<CropCommand>(Rect{100, 100, 10, 10}));  // no overlap
    PE_CHECK_EQ(doc->canvasSize().width, 32);  // degenerate crop leaves the canvas unchanged
    PE_CHECK_EQ(doc->canvasSize().height, 32);
    doc->history().undo();  // must not crash / corrupt
    PE_CHECK_EQ(doc->canvasSize().width, 32);
}

PE_TEST(crop_shifts_content_inside_groups) {
    // Crop must shift pixel content nested in groups, not just top-level layers — otherwise
    // the canvas would resize while grouped content stayed put (misaligned).
    auto doc = Document::createBlank(Size{64, 64});
    auto group = std::make_unique<GroupLayer>("Grp");
    auto child = std::make_unique<PixelLayer>("Inner");
    child->tiles().setPixel(30, 30, Rgba8{10, 200, 30, 255});
    const LayerId childId = child->id();
    group->addChild(std::move(child));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 10, 40, 40}));
    PE_CHECK_EQ(doc->canvasSize().width, 40);
    auto* inner = static_cast<PixelLayer*>(doc->findLayer(childId));
    PE_CHECK_EQ(inner->tiles().pixel(20, 20), (Rgba8{10, 200, 30, 255}));  // shifted -(10,10)
    PE_CHECK_EQ(inner->tiles().pixel(30, 30).a, static_cast<uint8_t>(0));

    doc->history().undo();
    PE_CHECK_EQ(doc->canvasSize().width, 64);
    PE_CHECK_EQ(static_cast<PixelLayer*>(doc->findLayer(childId))->tiles().pixel(30, 30),
                (Rgba8{10, 200, 30, 255}));
}

PE_TEST(crop_shifts_active_selection_with_content) {
    // An active selection must track the cropped content: it shifts by -origin, same as pixels.
    auto doc = Document::createBlank(Size{64, 64});
    doc->editableSelection().selectRect(Rect{20, 18, 10, 10});  // a 10x10 marquee
    PE_CHECK(doc->selection().active());
    PE_CHECK_EQ(doc->selection().value(25, 22), static_cast<uint8_t>(255));  // inside, pre-crop

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));
    PE_CHECK(doc->selection().active());
    // Selection shifts by -(10,8): the rect's top-left moves (20,18) -> (10,10).
    PE_CHECK_EQ(doc->selection().value(10, 10), static_cast<uint8_t>(255));  // new top-left, inside
    PE_CHECK_EQ(doc->selection().value(19, 19), static_cast<uint8_t>(255));  // new bottom-right
    PE_CHECK_EQ(doc->selection().value(25, 22), static_cast<uint8_t>(0));    // old spot vacated
    PE_CHECK_EQ(doc->selection().tightBounds(), (Rect{10, 10, 10, 10}));

    doc->history().undo();
    // Undo restores the original selection exactly (back at its pre-crop location).
    PE_CHECK(doc->selection().active());
    PE_CHECK_EQ(doc->selection().tightBounds(), (Rect{20, 18, 10, 10}));
    PE_CHECK_EQ(doc->selection().value(25, 22), static_cast<uint8_t>(255));
    PE_CHECK_EQ(doc->selection().value(10, 10), static_cast<uint8_t>(0));
}

PE_TEST(crop_leaves_inactive_selection_inactive) {
    // With no active selection, crop must not spuriously activate one.
    auto doc = Document::createBlank(Size{64, 64});
    PE_CHECK(!doc->selection().active());

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));
    PE_CHECK(!doc->selection().active());  // still inactive (whole canvas editable)

    doc->history().undo();
    PE_CHECK(!doc->selection().active());
}

PE_TEST(crop_refuses_when_content_exceeds_move_budget) {
    // A layer whose content exceeds the per-layer move budget can't be shifted; rather than
    // resize the canvas and leave content unmoved (half-cropped), the crop must be a no-op.
    auto doc = Document::createBlank(Size{5000, 5000});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().setPixel(10, 10, Rgba8{1, 2, 3, 255});
    pl->tiles().setPixel(4990, 4990, Rgba8{4, 5, 6, 255});  // content bbox ~5120x5120 (>16 MP)

    doc->history().push(std::make_unique<CropCommand>(Rect{1000, 1000, 1000, 1000}));
    PE_CHECK_EQ(doc->canvasSize().width, 5000);  // refused: canvas unchanged
    PE_CHECK_EQ(doc->canvasSize().height, 5000);
    PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{1, 2, 3, 255}));  // content not moved
}
