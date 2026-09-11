// Canvas Size: change the canvas rectangle without resampling, positioning the existing
// content by a 3x3 anchor. Shares its machinery with Crop (ReframeCommand), so the tests that
// matter here are the ones Crop cannot reach: growing, and the anchors.

#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <memory>

using namespace pe;

namespace {

std::unique_ptr<Document> docWith(Size size, Rgba8 fill) {
    auto doc = Document::createBlank(size);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, size.width, size.height}, fill);
    return doc;
}

const PixelLayer* pixels(const Document& doc) {
    return static_cast<const PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

void resize(Document& doc, Size to, CanvasAnchor anchor) {
    doc.history().push(std::make_unique<ResizeCanvasCommand>(to, anchor));
}

}  // namespace

PE_TEST(canvas_anchor_offset_is_the_whole_of_what_the_grid_means) {
    // Growing 100 -> 200 on both axes: the new space goes on the far side of the anchor.
    const Size from{100, 100};
    const Size to{200, 200};
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::TopLeft) == (Point{0, 0}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::Top) == (Point{50, 0}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::TopRight) == (Point{100, 0}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::Left) == (Point{0, 50}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::Center) == (Point{50, 50}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::Right) == (Point{100, 50}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::BottomLeft) == (Point{0, 100}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::Bottom) == (Point{50, 100}));
    PE_CHECK(canvasAnchorOffset(from, to, CanvasAnchor::BottomRight) == (Point{100, 100}));

    // Shrinking runs the same arithmetic with a negative growth: the anchor decides which part
    // of the picture stays on the canvas.
    PE_CHECK(canvasAnchorOffset(to, from, CanvasAnchor::TopLeft) == (Point{0, 0}));
    PE_CHECK(canvasAnchorOffset(to, from, CanvasAnchor::Center) == (Point{-50, -50}));
    PE_CHECK(canvasAnchorOffset(to, from, CanvasAnchor::BottomRight) == (Point{-100, -100}));

    // An odd difference puts the spare pixel on the right/bottom rather than splitting it.
    PE_CHECK(canvasAnchorOffset(Size{100, 100}, Size{101, 101}, CanvasAnchor::Center) ==
             (Point{0, 0}));
    PE_CHECK(canvasAnchorOffset(Size{100, 100}, Size{103, 103}, CanvasAnchor::Center) ==
             (Point{1, 1}));
}

PE_TEST(canvas_size_growing_from_the_top_left_moves_no_pixels_at_all) {
    // The cheap case, and the reason Canvas Size is not Image Size: tile coordinates are
    // absolute document space and nothing clips content to the canvas, so growing is a
    // metadata change. The content must stay exactly where it was.
    auto doc = docWith(Size{64, 64}, Rgba8{200, 100, 50, 255});
    const std::size_t tilesBefore = pixels(*doc)->tiles().tileCount();

    resize(*doc, Size{256, 128}, CanvasAnchor::TopLeft);

    PE_CHECK(doc->canvasSize() == (Size{256, 128}));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(0, 0).r, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(63, 63).r, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(64, 64).a, static_cast<uint8_t>(0));  // new space
    // No tile was written, split or materialized: nothing moved.
    PE_CHECK_EQ(pixels(*doc)->tiles().tileCount(), tilesBefore);
}

PE_TEST(canvas_size_growing_centred_shifts_the_content_into_the_middle) {
    auto doc = docWith(Size{64, 64}, Rgba8{200, 100, 50, 255});

    resize(*doc, Size{128, 128}, CanvasAnchor::Center);

    PE_CHECK(doc->canvasSize() == (Size{128, 128}));
    // The 64px square now sits at 32..95 on both axes.
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(32, 32).r, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(95, 95).r, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(31, 31).a, static_cast<uint8_t>(0));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(96, 96).a, static_cast<uint8_t>(0));
}

PE_TEST(canvas_size_shrinking_does_not_destroy_what_falls_outside) {
    // Unlike a crop of the same shape. The tile store is sparse and unbounded and .pedoc keeps
    // off-canvas content deliberately, so a shrink is exactly reversible and the pixels are
    // still there to come back.
    auto doc = docWith(Size{128, 128}, Rgba8{10, 200, 30, 255});

    resize(*doc, Size{64, 64}, CanvasAnchor::TopLeft);

    PE_CHECK(doc->canvasSize() == (Size{64, 64}));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(10, 10).g, static_cast<uint8_t>(200));
    // Outside the new canvas, and still present.
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(100, 100).g, static_cast<uint8_t>(200));

    doc->history().undo();
    PE_CHECK(doc->canvasSize() == (Size{128, 128}));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(100, 100).g, static_cast<uint8_t>(200));
}

PE_TEST(canvas_size_undoes_back_to_the_size_and_the_positions_it_had) {
    auto doc = docWith(Size{64, 64}, Rgba8{200, 100, 50, 255});

    resize(*doc, Size{200, 160}, CanvasAnchor::BottomRight);
    PE_REQUIRE(doc->canvasSize() == (Size{200, 160}));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(136, 96).r, static_cast<uint8_t>(200));  // 200-64

    doc->history().undo();

    PE_CHECK(doc->canvasSize() == (Size{64, 64}));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(0, 0).r, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(63, 63).r, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(136, 96).a, static_cast<uint8_t>(0));
}

PE_TEST(canvas_size_carries_masks_text_origins_and_fill_bounds_with_the_pixels) {
    // Everything in document space moves together, or the resize lines a mask up with the
    // wrong pixels. This is the half a naive "just change the canvas rect" implementation
    // leaves behind.
    auto doc = docWith(Size{64, 64}, Rgba8{200, 100, 50, 255});
    Layer* base = doc->findLayer(doc->activeLayer());
    auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
    mask->buffer().fillRect(Rect{0, 0, 32, 64}, MaskBuffer::kClear);  // hide the left half
    base->setMask(std::move(mask));

    auto fill =
        std::make_unique<SolidColorLayer>(Rgba8{0, 0, 255, 255}, Rect{4, 8, 16, 16}, "Fill");
    const LayerId fillId = fill->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(fill));

    resize(*doc, Size{128, 128}, CanvasAnchor::Center);  // offset (32, 32)

    // The mask moved with its layer: what was hidden at x<32 is now hidden at x<64.
    PE_CHECK(base->mask()->evaluate(40, 40) < 0.01f);
    PE_CHECK(base->mask()->evaluate(70, 40) > 0.99f);
    // And the fill layer's bounds moved by the same offset.
    const auto* f = static_cast<const SolidColorLayer*>(doc->findLayer(fillId));
    PE_CHECK(f->bounds() == (Rect{36, 40, 16, 16}));

    doc->history().undo();
    PE_CHECK(base->mask()->evaluate(8, 40) < 0.01f);
    PE_CHECK(static_cast<const SolidColorLayer*>(doc->findLayer(fillId))->bounds() ==
             (Rect{4, 8, 16, 16}));
}

PE_TEST(canvas_size_moves_the_selection_with_the_content) {
    auto doc = docWith(Size{64, 64}, Rgba8{200, 100, 50, 255});
    Selection sel;
    sel.selectRect(Rect{8, 8, 16, 16});
    doc->history().push(std::make_unique<SetSelectionCommand>(std::move(sel)));

    resize(*doc, Size{128, 128}, CanvasAnchor::Center);  // offset (32, 32)

    PE_REQUIRE(doc->selection().active());
    PE_CHECK_EQ(doc->selection().value(48, 48), 255);  // 8 + 32, still selected
    PE_CHECK_EQ(doc->selection().value(10, 10), 0);    // where it used to be, no longer

    doc->history().undo();
    PE_CHECK_EQ(doc->selection().value(10, 10), 255);
}

PE_TEST(canvas_resize_blocker_names_what_stopped_it) {
    auto doc = docWith(Size{64, 64}, Rgba8{1, 2, 3, 255});
    PE_CHECK(canvasResizeBlocker(*doc, Size{128, 128}, CanvasAnchor::Center) ==
             CanvasResizeBlock::None);
    PE_CHECK(canvasResizeBlocker(*doc, Size{64, 64}, CanvasAnchor::Center) ==
             CanvasResizeBlock::Unchanged);
    PE_CHECK(canvasResizeBlocker(*doc, Size{0, 64}, CanvasAnchor::TopLeft) ==
             CanvasResizeBlock::DimensionOutOfRange);
    PE_CHECK(canvasResizeBlocker(*doc, Size{-5, 64}, CanvasAnchor::TopLeft) ==
             CanvasResizeBlock::DimensionOutOfRange);
    PE_CHECK(canvasResizeBlocker(*doc, Size{kMaxCanvasDimension + 1, 64}, CanvasAnchor::TopLeft) ==
             CanvasResizeBlock::DimensionOutOfRange);
    // A top-left anchor moves nothing, so it is never blocked on shiftability.
    PE_CHECK(canvasResizeBlocker(*doc, Size{200000, 200000}, CanvasAnchor::TopLeft) ==
             CanvasResizeBlock::None);
}

PE_TEST(canvas_size_that_changes_nothing_touches_nothing) {
    auto doc = docWith(Size{64, 64}, Rgba8{1, 2, 3, 255});
    auto cmd = std::make_unique<ResizeCanvasCommand>(Size{64, 64}, CanvasAnchor::Center);
    auto* raw = cmd.get();
    doc->history().push(std::move(cmd));
    PE_CHECK(!raw->reframed());
    PE_CHECK(doc->canvasSize() == (Size{64, 64}));
}

PE_TEST(canvas_size_and_crop_report_the_bytes_they_are_holding) {
    // Both hold per-layer content moves plus a selection snapshot for undo. Crop reported the
    // base class's zero until these shared a base, which let History carry them believing its
    // stacks were empty.
    auto doc = docWith(Size{512, 512}, Rgba8{200, 100, 50, 255});
    Selection sel;
    sel.selectRect(Rect{0, 0, 400, 400});
    doc->history().push(std::make_unique<SetSelectionCommand>(std::move(sel)));

    // Anchoring top left moves nothing, so this one holds the selection snapshot and no
    // content moves at all. It is the baseline the moving case has to beat.
    ResizeCanvasCommand stillCmd(Size{600, 600}, CanvasAnchor::TopLeft);
    PE_CHECK_EQ(stillCmd.retainedBytes(), static_cast<std::int64_t>(0));  // nothing held yet
    (void)stillCmd.execute(*doc);
    PE_REQUIRE(stillCmd.reframed());
    const std::int64_t selectionOnly = stillCmd.retainedBytes();
    PE_CHECK(selectionOnly > 0);  // the selection snapshot is real and resident
    (void)stillCmd.undo(*doc);

    // The same resize anchored centrally moves every pixel, and those moves are the bulk of
    // what it holds. Asserting only "> 0" would pass while the moves went uncounted.
    ResizeCanvasCommand resizeCmd(Size{600, 600}, CanvasAnchor::Center);
    (void)resizeCmd.execute(*doc);
    PE_REQUIRE(resizeCmd.reframed());
    PE_CHECK(resizeCmd.retainedBytes() > selectionOnly);
    // A 512x512 8-bit layer is 4 tiles of 256 KiB, doubled because a move records before and
    // after, so the content alone dwarfs the selection's 1 byte per pixel.
    PE_CHECK(resizeCmd.retainedBytes() > 1024LL * 1024);
    (void)resizeCmd.undo(*doc);

    CropCommand cropCmd(Rect{10, 10, 200, 200});
    (void)cropCmd.execute(*doc);
    PE_REQUIRE(cropCmd.reframed());
    PE_CHECK(cropCmd.retainedBytes() > 0);
}

PE_TEST(canvas_size_redoes_after_an_undo) {
    auto doc = docWith(Size{64, 64}, Rgba8{200, 100, 50, 255});
    resize(*doc, Size{128, 128}, CanvasAnchor::Center);
    doc->history().undo();
    PE_REQUIRE(doc->canvasSize() == (Size{64, 64}));
    doc->history().redo();
    PE_CHECK(doc->canvasSize() == (Size{128, 128}));
    PE_CHECK_EQ(pixels(*doc)->tiles().pixel(32, 32).r, static_cast<uint8_t>(200));
}
