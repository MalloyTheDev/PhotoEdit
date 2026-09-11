// Image Size: ResampleDocumentCommand scales the whole document -- canvas, every pixel layer
// (groups included), layer masks, text rasters, fill bounds and the selection -- as one undoable
// step. It is a SIBLING of ReframeCommand, so these tests are the resample analogues of the crop
// tests: the per-piece resamplers are pinned in their own suites, and what matters here is that the
// command drives all of them together, refuses all-or-nothing, and undoes exactly.

#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe/core/TextLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <memory>

using namespace pe;

namespace {

std::unique_ptr<Document> gradientDoc(int w, int h) {
    auto doc = Document::createBlank(Size{w, h});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            pl->tiles().setPixel(x, y,
                                 Rgba8{static_cast<uint8_t>(x * 255 / (w - 1)),
                                       static_cast<uint8_t>(y * 255 / (h - 1)),
                                       static_cast<uint8_t>((x + y) * 255 / (w + h - 2)), 255});
        }
    }
    return doc;
}

PixelLayer* base(Document& doc) {
    return static_cast<PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

// Push a resample and hand back the raw command so the test can read resampled()/retainedBytes().
ResampleDocumentCommand* resize(Document& doc, Size to) {
    auto cmd = std::make_unique<ResampleDocumentCommand>(to);
    auto* raw = cmd.get();
    doc.history().push(std::move(cmd));
    return raw;
}

}  // namespace

PE_TEST(imageresize_blocker_reports_why_it_cannot_run) {
    auto doc = Document::createBlank(Size{64, 64});
    PE_CHECK(imageResizeBlocker(*doc, Size{64, 64}) == ImageResizeBlock::Unchanged);
    PE_CHECK(imageResizeBlocker(*doc, Size{0, 64}) == ImageResizeBlock::DimensionOutOfRange);
    PE_CHECK(imageResizeBlocker(*doc, Size{64, -1}) == ImageResizeBlock::DimensionOutOfRange);
    PE_CHECK(imageResizeBlocker(*doc, Size{kMaxCanvasDimension + 1, 64}) ==
             ImageResizeBlock::DimensionOutOfRange);
    PE_CHECK(imageResizeBlocker(*doc, Size{128, 96}) == ImageResizeBlock::None);
}

PE_TEST(imageresize_scales_canvas_and_pixels_undoable) {
    auto doc = gradientDoc(64, 64);
    const Rgba8 a = base(*doc)->tiles().pixel(10, 10);
    const Rgba8 b = base(*doc)->tiles().pixel(63, 40);

    auto* cmd = resize(*doc, Size{128, 128});
    PE_CHECK(cmd->resampled());
    PE_CHECK(doc->canvasSize() == (Size{128, 128}));
    // Content doubled: a pixel near (64,80) is opaque where the source had content.
    PE_CHECK_EQ(base(*doc)->tiles().pixel(64, 80).a, static_cast<uint8_t>(255));

    doc->history().undo();
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));
    PE_CHECK(base(*doc)->tiles().pixel(10, 10) == a);  // restored exactly
    PE_CHECK(base(*doc)->tiles().pixel(63, 40) == b);
}

PE_TEST(imageresize_unchanged_size_is_a_noop) {
    auto doc = gradientDoc(64, 64);
    auto* cmd = resize(*doc, Size{64, 64});
    PE_CHECK(!cmd->resampled());
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));  // untouched
    doc->history().undo();                          // must not corrupt
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));
}

PE_TEST(imageresize_dimension_out_of_range_is_a_noop) {
    auto doc = gradientDoc(64, 64);
    auto* cmd = resize(*doc, Size{0, 128});
    PE_CHECK(!cmd->resampled());
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));
}

PE_TEST(imageresize_downscale_shrinks_canvas_and_keeps_content) {
    auto doc = gradientDoc(128, 128);
    PE_REQUIRE(base(*doc)->tiles().pixel(100, 100).a == 255);
    resize(*doc, Size{32, 32});
    PE_CHECK(doc->canvasSize() == (Size{32, 32}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(10, 10).a, static_cast<uint8_t>(255));  // content here
    PE_CHECK_EQ(base(*doc)->tiles().pixel(100, 100).a, static_cast<uint8_t>(0));  // vacated
    doc->history().undo();
    PE_CHECK(doc->canvasSize() == (Size{128, 128}));
    PE_CHECK_EQ(base(*doc)->tiles().pixel(100, 100).a, static_cast<uint8_t>(255));
}

PE_TEST(imageresize_scales_a_layer_mask_with_the_pixels) {
    auto doc = gradientDoc(64, 64);
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{0, 0, 16, 64}, MaskBuffer::kClear);  // hide the left quarter
    base(*doc)->setMask(std::move(mask));

    resize(*doc, Size{128, 128});  // 2x: the hidden quarter must move out to ~[0,32)
    const MaskBuffer& mb = base(*doc)->mask()->buffer();
    // x=20 discriminates a scaled mask from an unchanged one: only after the [0,16) hide is
    // resampled to ~[0,32) is x=20 still hidden (an unscaled mask reads absent == revealing there).
    PE_CHECK_EQ(mb.value(20, 40), MaskBuffer::kClear);   // hide boundary moved out with the pixels
    PE_CHECK_EQ(mb.value(80, 40), MaskBuffer::kOpaque);  // right side revealed

    doc->history().undo();
    const MaskBuffer& back = base(*doc)->mask()->buffer();
    PE_CHECK_EQ(back.value(5, 40), MaskBuffer::kClear);    // restored to the 64-wide layout
    PE_CHECK_EQ(back.value(20, 40), MaskBuffer::kOpaque);  // x=20 outside the [0,16) hide again
}

PE_TEST(imageresize_scales_a_text_layer) {
    auto doc = gradientDoc(64, 64);
    TextModel tm;
    tm.pixelSize = 50;
    tm.origin = Point{20, 18};
    PixelBuffer raster(4, 6, Rgba8{10, 20, 30, 255});
    auto text = std::make_unique<TextLayer>(tm, std::move(raster), Point{20, 18});
    const LayerId id = text->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(text));

    resize(*doc, Size{128, 128});  // 2x on both axes
    auto* t = static_cast<TextLayer*>(doc->findLayer(id));
    PE_REQUIRE(t != nullptr);
    PE_CHECK_EQ(t->raster().width(), 8);    // 4 -> 8
    PE_CHECK_EQ(t->raster().height(), 12);  // 6 -> 12
    PE_CHECK_EQ(t->rasterOrigin().x, 40);
    PE_CHECK_EQ(t->rasterOrigin().y, 36);
    PE_CHECK_EQ(t->model().pixelSize, 100);          // font size scaled
    PE_CHECK(t->model().origin == (Point{40, 36}));  // placement scaled

    doc->history().undo();
    t = static_cast<TextLayer*>(doc->findLayer(id));
    PE_REQUIRE(t != nullptr);
    PE_CHECK_EQ(t->raster().width(), 4);
    PE_CHECK_EQ(t->raster().height(), 6);
    PE_CHECK_EQ(t->rasterOrigin().x, 20);
    PE_CHECK_EQ(t->model().pixelSize, 50);
}

PE_TEST(imageresize_scales_a_solid_color_layer_bounds) {
    auto doc = gradientDoc(64, 64);
    auto fill = std::make_unique<SolidColorLayer>(Rgba8{0, 128, 255, 255}, Rect{20, 18, 12, 10});
    const LayerId id = fill->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(fill));

    resize(*doc, Size{128, 128});  // 2x
    auto* f = static_cast<SolidColorLayer*>(doc->findLayer(id));
    PE_REQUIRE(f != nullptr);
    PE_CHECK(f->bounds() == (Rect{40, 36, 24, 20}));

    doc->history().undo();
    f = static_cast<SolidColorLayer*>(doc->findLayer(id));
    PE_REQUIRE(f != nullptr);
    PE_CHECK(f->bounds() == (Rect{20, 18, 12, 10}));
}

PE_TEST(imageresize_scales_the_selection) {
    auto doc = gradientDoc(100, 100);
    doc->editableSelection().selectRect(Rect{10, 10, 20, 20});
    doc->touchSelection();
    PE_REQUIRE(doc->selection().active());

    resize(*doc, Size{200, 200});  // 2x
    PE_REQUIRE(doc->selection().active());
    PE_CHECK(doc->selection().tightBounds() == (Rect{20, 20, 40, 40}));

    doc->history().undo();
    PE_REQUIRE(doc->selection().active());
    PE_CHECK(doc->selection().tightBounds() == (Rect{10, 10, 20, 20}));
}

PE_TEST(imageresize_scales_content_inside_groups) {
    auto doc = Document::createBlank(Size{64, 64});
    auto group = std::make_unique<GroupLayer>("Grp");
    auto child = std::make_unique<PixelLayer>("Inner");
    child->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{10, 200, 30, 255});
    const LayerId childId = child->id();
    group->addChild(std::move(child));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    resize(*doc, Size{128, 128});
    PE_CHECK(doc->canvasSize() == (Size{128, 128}));
    auto* inner = static_cast<PixelLayer*>(doc->findLayer(childId));
    PE_REQUIRE(inner != nullptr);
    PE_CHECK_EQ(inner->tiles().pixel(100, 100).a, static_cast<uint8_t>(255));  // content scaled up

    doc->history().undo();
    inner = static_cast<PixelLayer*>(doc->findLayer(childId));
    PE_CHECK_EQ(inner->tiles().pixel(100, 100).a, static_cast<uint8_t>(0));  // back to 64x64 extent
    PE_CHECK_EQ(inner->tiles().pixel(60, 60).a, static_cast<uint8_t>(255));
}

PE_TEST(imageresize_retained_bytes_counts_the_snapshots) {
    // History sums retainedBytes to bound its stacks; a resample that reported zero (the Command
    // default) while holding pixel deltas, a mask and a selection would let History carry them
    // believing the stacks were empty. So it must be strictly positive here.
    auto doc = gradientDoc(64, 64);
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{0, 0, 32, 64}, MaskBuffer::kClear);
    base(*doc)->setMask(std::move(mask));
    doc->editableSelection().selectRect(Rect{4, 4, 20, 20});
    doc->touchSelection();

    auto* cmd = resize(*doc, Size{128, 128});
    PE_CHECK(cmd->resampled());
    PE_CHECK(cmd->retainedBytes() > 0);
}

PE_TEST(imageresize_undo_then_redo_round_trips) {
    auto doc = gradientDoc(64, 64);
    resize(*doc, Size{100, 80});
    const Rgba8 afterExec = base(*doc)->tiles().pixel(40, 30);
    PE_CHECK(doc->canvasSize() == (Size{100, 80}));

    doc->history().undo();
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));

    doc->history().redo();
    PE_CHECK(doc->canvasSize() == (Size{100, 80}));
    PE_CHECK(base(*doc)->tiles().pixel(40, 30) == afterExec);  // redo reproduces execute exactly
}

PE_TEST(imageresize_refuses_when_a_text_raster_would_exceed_its_cap) {
    // A text raster cannot scale past kMaxTextRasterDim, or the saved .pedoc would be rejected on
    // reopen. When an upscale would, the whole resize refuses rather than resize-and-clip the text.
    auto doc = gradientDoc(64, 64);
    PixelBuffer wide(5000, 2, Rgba8{10, 20, 30, 255});  // valid now; 2x would make it 10000 wide
    auto text = std::make_unique<TextLayer>(TextModel{}, std::move(wide), Point{0, 0});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(text));

    PE_CHECK(imageResizeBlocker(*doc, Size{128, 128}) == ImageResizeBlock::ContentTooLarge);
    auto* cmd = resize(*doc, Size{128, 128});
    PE_CHECK(!cmd->resampled());
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));  // untouched
}
