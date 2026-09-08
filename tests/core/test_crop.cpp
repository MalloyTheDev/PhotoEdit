#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
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

PE_TEST(crop_preserves_sparse_selection_with_huge_bbox) {
    // A sparse selection (tiny coverage, far-apart rects) can have a bounding box that exceeds the
    // toMask cap. A crop must not silently deactivate it via an empty mask round-trip — it is kept
    // as-is (untranslated) rather than dropped.
    auto doc = Document::createBlank(Size{64, 64});
    doc->editableSelection().selectRect(Rect{0, 0, 16, 16});
    doc->editableSelection().addRect(Rect{100000, 100000, 16, 16});  // bbox ~100016^2 > 64 MP cap
    PE_CHECK(doc->selection().active());

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));
    PE_CHECK(doc->selection().active());  // preserved, not silently dropped

    doc->history().undo();
    PE_CHECK(doc->selection().active());
}

PE_TEST(crop_refuses_when_content_exceeds_move_budget) {
    // A layer whose content exceeds the per-layer move budget can't be shifted; rather than
    // resize the canvas and leave content unmoved (half-cropped), the crop must be a no-op.
    //
    // The budget is now kMaxMoveTiles (4096 tiles), not an area in pixels: a Move is built
    // one tile at a time at native depth, so what bounds it is the tiles it touches and the
    // undo record they produce. This case used to use a 5120x5120 bbox, which was over the
    // old 16 MP area cap; that document is now perfectly movable, and is asserted as such by
    // crop_moves_content_that_the_old_area_cap_refused below. Content spanning 79x79 tiles
    // is over the new bound.
    auto doc = Document::createBlank(Size{20000, 20000});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().setPixel(10, 10, Rgba8{1, 2, 3, 255});
    pl->tiles().setPixel(19990, 19990, Rgba8{4, 5, 6, 255});  // bbox ~20224^2 = 6241 tiles

    doc->history().push(std::make_unique<CropCommand>(Rect{1000, 1000, 1000, 1000}));
    PE_CHECK_EQ(doc->canvasSize().width, 20000);  // refused: canvas unchanged
    PE_CHECK_EQ(doc->canvasSize().height, 20000);
    PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{1, 2, 3, 255}));  // content not moved
}

PE_TEST(crop_moves_content_that_the_old_area_cap_refused) {
    // The behaviour change, asserted deliberately rather than left to a test that stopped
    // failing. A 5000x5000 document's content is a ~5120x5120 bbox: over the old 16 MP area
    // cap, so a crop here used to be refused outright and the user's crop silently did
    // nothing. It is 400 tiles, well inside the move budget, so it now works.
    auto doc = Document::createBlank(Size{5000, 5000});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().setPixel(10, 10, Rgba8{1, 2, 3, 255});
    pl->tiles().setPixel(4990, 4990, Rgba8{4, 5, 6, 255});

    doc->history().push(std::make_unique<CropCommand>(Rect{1000, 1000, 1000, 1000}));
    PE_CHECK_EQ(doc->canvasSize().width, 1000);
    PE_CHECK_EQ(doc->canvasSize().height, 1000);
    // Both pixels shifted by (-1000, -1000): the first lands off-canvas, the second at 3990.
    PE_CHECK_EQ(pl->tiles().pixel(-990, -990), (Rgba8{1, 2, 3, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(3990, 3990), (Rgba8{4, 5, 6, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{0, 0, 0, 0}));  // vacated

    doc->history().undo();
    PE_CHECK_EQ(doc->canvasSize().width, 5000);
    PE_CHECK_EQ(pl->tiles().pixel(10, 10), (Rgba8{1, 2, 3, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(4990, 4990), (Rgba8{4, 5, 6, 255}));
}

// ---------------------------------------------------------------------------
// Non-pixel document-space geometry. Crop shifted pixel content but left masks,
// text raster origins and fill bounds at their pre-crop coordinates, so every
// masked pixel, glyph and fill rect ended up offset from the raster it belonged
// to. The corruption was invisible in the tests above because none of them used
// anything but a plain pixel layer.
// ---------------------------------------------------------------------------

PE_TEST(crop_shifts_a_layer_mask_with_its_pixels) {
    auto doc = Document::createBlank(Size{64, 64});
    PixelLayer* pl = base(*doc);
    pl->tiles().setPixel(20, 18, Rgba8{200, 50, 50, 255});

    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{20, 18, 4, 4}, MaskBuffer::kClear);  // hide the mark
    pl->setMask(std::move(mask));

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));

    // The mark moved to (10,10); the mask that hides it must have moved with it.
    const MaskBuffer& mb = base(*doc)->mask()->buffer();
    PE_CHECK_EQ(static_cast<int>(mb.value(10, 10)), static_cast<int>(MaskBuffer::kClear));
    // ...and must no longer be hiding whatever now sits at the old coordinates.
    PE_CHECK_EQ(static_cast<int>(mb.value(20, 18)), static_cast<int>(MaskBuffer::kOpaque));

    doc->history().undo();
    const MaskBuffer& back = base(*doc)->mask()->buffer();
    PE_CHECK_EQ(static_cast<int>(back.value(20, 18)), static_cast<int>(MaskBuffer::kClear));
    PE_CHECK_EQ(static_cast<int>(back.value(10, 10)), static_cast<int>(MaskBuffer::kOpaque));
}

PE_TEST(crop_shifts_a_mask_on_a_document_the_old_area_cap_refused) {
    // A crop refuses as a WHOLE when any part of the geometry cannot be shifted, so the mask
    // budget must never be tighter than the pixel-move budget. It briefly was: the move
    // budget became a byte budget and this one stayed at the old 16 MP area cap, so a
    // 5000x5000 document could have its pixels shifted and not its masks. One mask then made
    // the crop refuse outright and the #180 fix evaporated for any masked document, which is
    // most real ones.
    auto doc = Document::createBlank(Size{5000, 5000});
    PE_REQUIRE(doc != nullptr);
    PixelLayer* pl = base(*doc);
    pl->tiles().setPixel(1200, 1200, Rgba8{200, 50, 50, 255});
    pl->tiles().setPixel(4990, 4990, Rgba8{9, 9, 9, 255});  // bbox ~5120x5120, over 16 MP

    // The mask must SPAN a large box, not merely exist: canTranslate budgets the mask's
    // tile-granular contentBounds, so a mask with one small rect never approaches the cap
    // however big the document is. Two marks at opposite corners give a ~5120x5120 box,
    // which is over the old 16 MP number and under the current one.
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{1200, 1200, 4, 4}, MaskBuffer::kClear);
    mask->buffer().setValue(4995, 4995, MaskBuffer::kClear);
    pl->setMask(std::move(mask));
    PE_CHECK(static_cast<std::int64_t>(pl->mask()->buffer().contentBounds().width) *
                 pl->mask()->buffer().contentBounds().height >
             16'000'000);

    // A shift that is NOT a whole-tile multiple, so canTranslate takes the budgeted path
    // rather than the free rekey.
    doc->history().push(std::make_unique<CropCommand>(Rect{1000, 1000, 1000, 1000}));
    PE_CHECK_EQ(doc->canvasSize().width, 1000);  // the crop happened at all
    PE_CHECK_EQ(base(*doc)->tiles().pixel(200, 200), (Rgba8{200, 50, 50, 255}));
    // And the mask moved with the pixels rather than staying at the pre-crop coordinates.
    const MaskBuffer& mb = base(*doc)->mask()->buffer();
    PE_CHECK_EQ(static_cast<int>(mb.value(200, 200)), static_cast<int>(MaskBuffer::kClear));
    PE_CHECK_EQ(static_cast<int>(mb.value(1200, 1200)), static_cast<int>(MaskBuffer::kOpaque));

    doc->history().undo();
    PE_CHECK_EQ(doc->canvasSize().width, 5000);
    PE_CHECK_EQ(static_cast<int>(base(*doc)->mask()->buffer().value(1200, 1200)),
                static_cast<int>(MaskBuffer::kClear));
}

PE_TEST(crop_shifts_a_text_layer_raster_origin) {
    auto doc = Document::createBlank(Size{64, 64});
    PixelBuffer raster(4, 4, Rgba8{10, 20, 30, 255});
    auto text = std::make_unique<TextLayer>(TextModel{}, std::move(raster), Point{20, 18});
    const LayerId id = text->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(text));

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));

    auto* t = static_cast<TextLayer*>(doc->findLayer(id));
    PE_CHECK(t != nullptr);
    if (t == nullptr) return;
    PE_CHECK_EQ(t->rasterOrigin().x, 10);
    PE_CHECK_EQ(t->rasterOrigin().y, 10);

    doc->history().undo();
    t = static_cast<TextLayer*>(doc->findLayer(id));
    PE_CHECK(t != nullptr);
    if (t == nullptr) return;
    PE_CHECK_EQ(t->rasterOrigin().x, 20);
    PE_CHECK_EQ(t->rasterOrigin().y, 18);
}

PE_TEST(crop_shifts_a_solid_color_layer_bounds) {
    auto doc = Document::createBlank(Size{64, 64});
    auto fill = std::make_unique<SolidColorLayer>(Rgba8{0, 128, 255, 255}, Rect{20, 18, 12, 12});
    const LayerId id = fill->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(fill));

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));

    auto* f = static_cast<SolidColorLayer*>(doc->findLayer(id));
    PE_CHECK(f != nullptr);
    if (f == nullptr) return;
    PE_CHECK(f->bounds() == (Rect{10, 10, 12, 12}));

    doc->history().undo();
    f = static_cast<SolidColorLayer*>(doc->findLayer(id));
    PE_CHECK(f != nullptr);
    if (f == nullptr) return;
    PE_CHECK(f->bounds() == (Rect{20, 18, 12, 12}));
}

PE_TEST(crop_shifts_a_mask_on_a_layer_nested_in_a_group) {
    // Masks live on the base Layer, so any kind can carry one at any depth. The
    // pixel-content walk already recursed; the geometry walk has to as well.
    auto doc = Document::createBlank(Size{64, 64});
    auto inner = std::make_unique<PixelLayer>("inner");
    const LayerId innerId = inner->id();
    inner->tiles().setPixel(20, 18, Rgba8{9, 9, 9, 255});
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{20, 18, 4, 4}, MaskBuffer::kClear);
    inner->setMask(std::move(mask));

    auto group = std::make_unique<GroupLayer>("group");
    group->addChild(std::move(inner));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));

    auto* l = doc->findLayer(innerId);
    PE_CHECK(l != nullptr);
    if (l == nullptr || l->mask() == nullptr) {
        PE_CHECK(false);
        return;
    }
    PE_CHECK_EQ(static_cast<int>(l->mask()->buffer().value(10, 10)),
                static_cast<int>(MaskBuffer::kClear));
    PE_CHECK_EQ(static_cast<int>(l->mask()->buffer().value(20, 18)),
                static_cast<int>(MaskBuffer::kOpaque));
}

PE_TEST(crop_redo_reapplies_geometry_exactly_once) {
    // execute() runs again on redo, so a geometry shift that accumulated instead of
    // being recomputed would drift further on every undo/redo cycle.
    auto doc = Document::createBlank(Size{64, 64});
    auto fill = std::make_unique<SolidColorLayer>(Rgba8{0, 128, 255, 255}, Rect{20, 18, 12, 12});
    const LayerId id = fill->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(fill));

    doc->history().push(std::make_unique<CropCommand>(Rect{10, 8, 30, 30}));
    for (int i = 0; i < 3; ++i) {
        doc->history().undo();
        doc->history().redo();
    }
    auto* f = static_cast<SolidColorLayer*>(doc->findLayer(id));
    PE_CHECK(f != nullptr);
    if (f == nullptr) return;
    PE_CHECK(f->bounds() == (Rect{10, 10, 12, 12}));
}
