#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/NativeFormat.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/TextLayer.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace pe;

namespace {
// A w x h raster filled with one opaque color (a stand-in for the app's rasterized glyphs).
PixelBuffer solidRaster(int w, int h, Rgba8 c) {
    PixelBuffer b(w, h);
    b.fill(c);
    return b;
}

TextModel sampleModel(std::string text) {
    return TextModel{std::move(text),        "Arial",    48, /*bold=*/true, /*italic=*/false,
                     Rgba8{10, 20, 30, 255}, Point{7, 9}};
}

Rgba8 cpx(Document& doc, int x, int y) {
    return doc.compositeImage().at(x, y);
}

// Insert a text layer at the top of a fresh blank doc; returns {doc, layerId}.
struct DocWithText {
    std::unique_ptr<Document> doc;
    LayerId id;
};
DocWithText makeDocWithText(PixelBuffer raster, Point rasterOrigin) {
    auto doc = Document::createBlank(Size{64, 48});
    auto layer =
        std::make_unique<TextLayer>(sampleModel("Hello"), std::move(raster), rasterOrigin, "Hello");
    const LayerId id = layer->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(layer));
    doc->setActiveLayer(id);
    return {std::move(doc), id};
}
}  // namespace

PE_TEST(textlayer_renders_raster_into_composite) {
    auto [doc, id] = makeDocWithText(solidRaster(4, 4, Rgba8{255, 0, 0, 255}), Point{10, 10});
    // Inside the raster placement rect -> the glyph color; outside -> the layer is transparent.
    const Rgba8 inside = cpx(*doc, 11, 11);
    PE_CHECK_EQ(static_cast<int>(inside.r), 255);
    PE_CHECK_EQ(static_cast<int>(inside.a), 255);
    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 0, 0).a), 0);    // far from the text -> nothing
    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 20, 20).a), 0);  // just past the 4x4 raster
}

PE_TEST(textlayer_content_bounds_tracks_raster) {
    auto [doc, id] = makeDocWithText(solidRaster(5, 6, Rgba8{1, 2, 3, 255}), Point{10, 12});
    const auto* t = static_cast<const TextLayer*>(doc->findLayer(id));
    const Rect cb = t->contentBounds();
    PE_CHECK_EQ(cb.x, 10);
    PE_CHECK_EQ(cb.y, 12);
    PE_CHECK_EQ(cb.width, 5);
    PE_CHECK_EQ(cb.height, 6);

    TextLayer empty(sampleModel(""), PixelBuffer{}, Point{3, 3});
    PE_CHECK(empty.contentBounds().isEmpty());  // no raster -> no content
}

PE_TEST(textlayer_clone_is_deep_and_fresh_id) {
    TextLayer src(sampleModel("Hi"), solidRaster(3, 3, Rgba8{9, 9, 9, 255}), Point{4, 5});
    src.setOpacity(0.5f);
    auto cl = src.clone();
    PE_CHECK(cl->id() != src.id());  // fresh identity
    const auto* t = static_cast<const TextLayer*>(cl.get());
    PE_CHECK(t->model() == src.model());  // model copied
    PE_CHECK(t->rasterOrigin() == src.rasterOrigin());
    PE_CHECK_EQ(t->raster().width(), 3);  // raster copied
    PE_CHECK_EQ(static_cast<int>(t->raster().at(1, 1).r), 9);
    PE_CHECK(t->opacity() == 0.5f);  // universal props copied
}

PE_TEST(textlayer_edit_swaps_and_undo_restores) {
    auto [doc, id] = makeDocWithText(solidRaster(4, 4, Rgba8{255, 0, 0, 255}), Point{10, 10});
    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 11, 11).r), 255);  // red text present

    // Edit: a different string + a blue raster placed elsewhere.
    doc->history().push(std::make_unique<EditTextCommand>(
        id, sampleModel("Bye"), solidRaster(6, 6, Rgba8{0, 0, 255, 255}), Point{30, 20}));

    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 11, 11).a), 0);    // old text vacated
    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 31, 21).b), 255);  // new blue text present
    PE_CHECK_EQ(doc->history().undoDepth(), 1u);
    PE_CHECK_EQ(doc->history().topUndoName(), std::string("Edit Text"));
    PE_CHECK(static_cast<const TextLayer*>(doc->findLayer(id))->model().text == "Bye");

    doc->history().undo();
    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 11, 11).r), 255);  // red restored
    PE_CHECK_EQ(static_cast<int>(cpx(*doc, 31, 21).a), 0);    // blue gone
    PE_CHECK(static_cast<const TextLayer*>(doc->findLayer(id))->model().text == "Hello");
}

PE_TEST(textlayer_roundtrip_preserves_model_and_raster) {
    PixelBuffer raster = solidRaster(8, 5, Rgba8{40, 80, 120, 200});
    raster.set(0, 0, Rgba8{255, 254, 253, 252});  // a distinct corner pixel to catch byte drift
    auto [doc, id] = makeDocWithText(std::move(raster), Point{12, 7});

    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(!blob.empty());
    auto loaded = deserializeDocument(blob);
    PE_CHECK(loaded != nullptr);

    const auto* t = dynamic_cast<const TextLayer*>(loaded->topLevelLayers().back().get());
    PE_CHECK(t != nullptr);
    PE_CHECK(t->model() == sampleModel("Hello"));  // model round-trips exactly
    PE_CHECK(t->rasterOrigin() == Point{12, 7});
    PE_CHECK_EQ(t->raster().width(), 8);
    PE_CHECK_EQ(t->raster().height(), 5);
    const Rgba8 corner = t->raster().at(0, 0);
    PE_CHECK_EQ(static_cast<int>(corner.r), 255);
    PE_CHECK_EQ(static_cast<int>(corner.a), 252);
    const Rgba8 mid = t->raster().at(4, 2);
    PE_CHECK_EQ(static_cast<int>(mid.r), 40);
    PE_CHECK_EQ(static_cast<int>(mid.a), 200);
}

PE_TEST(textlayer_roundtrip_allows_offcanvas_raster) {
    // Text may legitimately hang off the canvas edge; the raster carries its own placement and is
    // validated independently of the canvas, so this must round-trip (not be rejected like a pixel
    // content rect would be).
    auto [doc, id] = makeDocWithText(solidRaster(20, 20, Rgba8{1, 2, 3, 255}), Point{-8, 40});
    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_CHECK(loaded != nullptr);
    const auto* t = dynamic_cast<const TextLayer*>(loaded->topLevelLayers().back().get());
    PE_CHECK(t != nullptr);
    PE_CHECK(t->rasterOrigin() == Point{-8, 40});
    PE_CHECK_EQ(t->raster().width(), 20);
}
