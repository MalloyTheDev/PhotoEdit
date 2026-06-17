#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
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
