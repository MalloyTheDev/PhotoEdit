// Region copy, clear and paste: the engine half of cut/copy/paste. No clipboard here, which is
// the point of the split - these are testable with no Qt and no window.

#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <memory>

using namespace pe;

namespace {

std::unique_ptr<Document> docWith(Rgba8 fill, Size size = Size{64, 64}) {
    auto doc = Document::createBlank(size);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, size.width, size.height}, fill);
    return doc;
}

const PixelLayer* pixelsOf(const Document& doc, LayerId id) {
    return static_cast<const PixelLayer*>(doc.findLayer(id));
}

}  // namespace

PE_TEST(copy_region_is_the_whole_canvas_with_no_selection) {
    auto doc = docWith(Rgba8{10, 20, 30, 255});
    PE_CHECK(copyRegionFor(*doc, nullptr) == doc->canvasBounds());
    Selection none;  // constructed inactive
    PE_CHECK(copyRegionFor(*doc, &none) == doc->canvasBounds());
}

PE_TEST(copy_region_is_the_selection_s_tight_bounds_not_its_tiles) {
    // selectedBounds snaps out to whole 256px tiles, so using it would drag a small
    // selection's neighbours along with it on every copy.
    auto doc = docWith(Rgba8{10, 20, 30, 255}, Size{600, 600});
    Selection sel;
    sel.selectRect(Rect{100, 120, 40, 30});
    const Rect region = copyRegionFor(*doc, &sel);
    PE_CHECK_EQ(region.x, 100);
    PE_CHECK_EQ(region.y, 120);
    PE_CHECK_EQ(region.width, 40);
    PE_CHECK_EQ(region.height, 30);
}

PE_TEST(copy_region_of_a_selection_that_selects_nothing_is_empty) {
    // The case the caller has to refuse rather than copy: an active selection with no
    // coverage would otherwise produce a zero-size buffer and a confusing empty paste.
    auto doc = docWith(Rgba8{10, 20, 30, 255});
    Selection sel;
    sel.selectRect(Rect{5, 5, 10, 10});
    sel.subtractRect(Rect{0, 0, 64, 64});
    PE_CHECK(copyRegionFor(*doc, &sel).isEmpty());
}

PE_TEST(copy_reads_the_layer_s_pixels_at_the_region_asked_for) {
    auto doc = Document::createBlank(Size{64, 64});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            pl->tiles().setPixel(
                x, y, Rgba8{static_cast<uint8_t>(x * 4), static_cast<uint8_t>(y * 4), 90, 255});
        }
    }
    const PixelBuffer got = copyLayerRegion(*doc, doc->activeLayer(), Rect{10, 20, 8, 4}, nullptr);
    PE_REQUIRE(!got.isEmpty());
    PE_CHECK_EQ(got.width(), 8);
    PE_CHECK_EQ(got.height(), 4);
    // Top-left of the buffer is the top-left of the REGION, not of the canvas.
    PE_CHECK_EQ(got.at(0, 0).r, static_cast<uint8_t>(40));
    PE_CHECK_EQ(got.at(0, 0).g, static_cast<uint8_t>(80));
    PE_CHECK_EQ(got.at(7, 3).r, static_cast<uint8_t>(68));
    PE_CHECK_EQ(got.at(7, 3).g, static_cast<uint8_t>(92));
}

PE_TEST(copy_folds_the_selection_into_alpha_rather_than_cutting_it_square) {
    // A feathered selection has to copy with a soft edge. Taking the bounding box at full
    // alpha would paste a rectangle, which is the wrong shape and obviously so.
    auto doc = docWith(Rgba8{200, 100, 50, 255});
    Selection sel;
    sel.selectRect(Rect{16, 16, 32, 32});
    sel.feather(4.0f, doc->canvasBounds());

    const Rect region = copyRegionFor(*doc, &sel);
    const PixelBuffer got = copyLayerRegion(*doc, doc->activeLayer(), region, &sel);
    PE_REQUIRE(!got.isEmpty());

    // Middle of the selection: fully selected, so fully opaque.
    PE_CHECK_EQ(got.at(got.width() / 2, got.height() / 2).a, static_cast<uint8_t>(255));
    // The colour is untouched wherever it survives: only alpha carries the selection.
    PE_CHECK_EQ(got.at(got.width() / 2, got.height() / 2).r, static_cast<uint8_t>(200));
    // The very edge of the feathered region is partly selected, so partly transparent.
    const int edge = got.at(0, got.height() / 2).a;
    PE_CHECK(edge < 255);
}

PE_TEST(copy_outside_the_layer_s_content_reads_as_transparent) {
    auto doc = Document::createBlank(Size{64, 64});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{255, 0, 0, 255});
    const PixelBuffer got = copyLayerRegion(*doc, doc->activeLayer(), Rect{0, 0, 16, 16}, nullptr);
    PE_REQUIRE(!got.isEmpty());
    PE_CHECK_EQ(got.at(1, 1).a, static_cast<uint8_t>(255));
    PE_CHECK_EQ(got.at(12, 12).a, static_cast<uint8_t>(0));
}

PE_TEST(copy_refuses_what_it_cannot_hold) {
    auto doc = docWith(Rgba8{1, 2, 3, 255});
    PE_CHECK(copyLayerRegion(*doc, doc->activeLayer(), Rect{}, nullptr).isEmpty());
    PE_CHECK(copyLayerRegion(*doc, kNoLayer, Rect{0, 0, 4, 4}, nullptr).isEmpty());
    // Absurd region: rejected before anything is allocated for it.
    PE_CHECK(
        copyLayerRegion(*doc, doc->activeLayer(), Rect{0, 0, 500000, 500000}, nullptr).isEmpty());
}

PE_TEST(clear_empties_the_region_and_undoes_back) {
    auto doc = docWith(Rgba8{200, 100, 50, 255});
    const LayerId id = doc->activeLayer();
    auto cmd = clearRegion(*doc, id, Rect{8, 8, 16, 16}, nullptr);
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(12, 12).a, static_cast<uint8_t>(0));
    // And nothing outside it moved.
    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(30, 30).a, static_cast<uint8_t>(255));
    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(30, 30).r, static_cast<uint8_t>(200));

    doc->history().undo();
    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(12, 12).a, static_cast<uint8_t>(255));
    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(12, 12).r, static_cast<uint8_t>(200));
}

PE_TEST(clear_leaves_no_colour_behind_the_transparency) {
    // A pixel with its old colour and zero alpha is invisible, not empty. Anything that later
    // raises alpha, or an export that ignores it, brings the old colour back.
    auto doc = docWith(Rgba8{200, 100, 50, 255});
    const LayerId id = doc->activeLayer();
    doc->history().push(clearRegion(*doc, id, Rect{0, 0, 8, 8}, nullptr));
    const Rgba8 cleared = pixelsOf(*doc, id)->tiles().pixel(4, 4);
    PE_CHECK_EQ(cleared.a, static_cast<uint8_t>(0));
    PE_CHECK_EQ(cleared.r, static_cast<uint8_t>(0));
    PE_CHECK_EQ(cleared.g, static_cast<uint8_t>(0));
    PE_CHECK_EQ(cleared.b, static_cast<uint8_t>(0));
}

PE_TEST(clear_honours_the_selection) {
    auto doc = docWith(Rgba8{200, 100, 50, 255});
    const LayerId id = doc->activeLayer();
    Selection sel;
    sel.selectRect(Rect{0, 0, 16, 64});  // the left quarter only
    doc->history().push(clearRegion(*doc, id, doc->canvasBounds(), &sel));

    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(4, 4).a, static_cast<uint8_t>(0));
    PE_CHECK_EQ(pixelsOf(*doc, id)->tiles().pixel(40, 4).a, static_cast<uint8_t>(255));
}

PE_TEST(paste_puts_the_buffer_where_it_is_told) {
    PixelBuffer src(4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) src.set(x, y, Rgba8{70, 80, 90, 255});
    }
    auto layer = layerFromBuffer(src, Point{20, 30}, "Pasted", nullptr);
    PE_REQUIRE(layer != nullptr);
    PE_CHECK(layer->name() == "Pasted");
    PE_CHECK_EQ(layer->tiles().pixel(20, 30).r, static_cast<uint8_t>(70));
    PE_CHECK_EQ(layer->tiles().pixel(23, 33).b, static_cast<uint8_t>(90));
    PE_CHECK_EQ(layer->tiles().pixel(19, 30).a, static_cast<uint8_t>(0));  // outside it
    PE_CHECK(layer->mask() == nullptr);  // no selection was handed in
}

PE_TEST(paste_of_a_sparse_shape_does_not_materialize_its_bounding_box) {
    // The tile store is sparse. Writing the transparent pixels too would make pasting a small
    // shape cost the same as pasting the rectangle around it.
    PixelBuffer src(600, 600);
    src.set(5, 5, Rgba8{255, 0, 0, 255});  // one opaque pixel in a large transparent field
    auto layer = layerFromBuffer(src, Point{0, 0}, "Sparse", nullptr);
    PE_REQUIRE(layer != nullptr);
    PE_CHECK_EQ(layer->tiles().tileCount(), static_cast<std::size_t>(1));
}

PE_TEST(paste_into_carries_the_selection_as_the_layer_s_mask) {
    // What Paste Into is: all the pixels arrive, and the selection decides how much shows, so
    // the paste can still be moved around inside the shape afterwards.
    PixelBuffer src(32, 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) src.set(x, y, Rgba8{10, 200, 10, 255});
    }
    Selection sel;
    sel.selectRect(Rect{0, 0, 16, 32});  // the left half of where it lands

    auto layer = layerFromBuffer(src, Point{0, 0}, "Pasted Into", &sel);
    PE_REQUIRE(layer != nullptr);
    PE_REQUIRE(layer->mask() != nullptr);
    // Every pixel is present...
    PE_CHECK_EQ(layer->tiles().pixel(20, 10).a, static_cast<uint8_t>(255));
    // ...and the mask is what hides the part outside the selection.
    PE_CHECK(layer->mask()->evaluate(4, 10) > 0.99f);
    PE_CHECK(layer->mask()->evaluate(20, 10) < 0.01f);
    // Outside the pasted region the mask is absent, which reads as revealing. That is correct
    // and harmless: there are no pixels out there to reveal.
    PE_CHECK(layer->mask()->evaluate(500, 500) > 0.99f);
}

PE_TEST(paste_refuses_an_empty_buffer) {
    // The size caps above this are not exercised here: reaching them needs a buffer that
    // cannot be allocated in a test. copy_refuses_what_it_cannot_hold covers the same guards
    // on the read side, where the region is a parameter rather than an allocation.
    PE_CHECK(layerFromBuffer(PixelBuffer{}, Point{0, 0}, "x", nullptr) == nullptr);
}

PE_TEST(copy_then_paste_reproduces_the_pixels) {
    // The round trip the whole feature is for.
    auto doc = docWith(Rgba8{123, 45, 67, 255});
    Selection sel;
    sel.selectRect(Rect{8, 8, 16, 16});
    const Rect region = copyRegionFor(*doc, &sel);
    const PixelBuffer copied = copyLayerRegion(*doc, doc->activeLayer(), region, &sel);
    PE_REQUIRE(!copied.isEmpty());

    auto pasted = layerFromBuffer(copied, Point{region.x, region.y}, "Pasted", nullptr);
    PE_REQUIRE(pasted != nullptr);
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            PE_CHECK(pasted->tiles().pixel(region.x + x, region.y + y) ==
                     pixelsOf(*doc, doc->activeLayer())->tiles().pixel(region.x + x, region.y + y));
        }
    }
}
