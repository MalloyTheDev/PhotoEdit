#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(document_blank_invariants) {
    auto doc = Document::createBlank(Size{100, 80});
    PE_CHECK(doc != nullptr);
    PE_CHECK_EQ(doc->canvasSize(), (Size{100, 80}));
    PE_CHECK_EQ(doc->canvasBounds(), (Rect{0, 0, 100, 80}));
    PE_CHECK_EQ(doc->resolutionPpi(), 72);
    PE_CHECK(doc->colorMode() == ColorMode::RGB);
    PE_CHECK(doc->bitDepth() == BitDepth::U8);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));  // one base layer
    PE_CHECK(doc->activeLayer() != kNoLayer);
    PE_CHECK(!doc->isDirty());

    // Base layer is a pixel layer with no tiles allocated (transparent).
    const Layer* base = doc->findLayer(doc->activeLayer());
    PE_CHECK(base != nullptr);
    PE_CHECK(base->kind() == LayerKind::Pixel);
    PE_CHECK(base->contentBounds().isEmpty());
}

PE_TEST(document_rejects_degenerate) {
    PE_CHECK(Document::createBlank(Size{0, 100}) == nullptr);
    PE_CHECK(Document::createBlank(Size{100, 0}) == nullptr);
    PE_CHECK(Document::createBlank(Size{-5, 5}) == nullptr);
    PE_CHECK(Document::createBlank(Size{kMaxCanvasDimension + 1, 10}) == nullptr);
    PE_CHECK(Document::createBlank(Size{10, 10}, ColorMode::RGB, BitDepth::U8, 0) == nullptr);
    // A maximal dimension is accepted (no eager allocation).
    PE_CHECK(Document::createBlank(Size{kMaxCanvasDimension, 1}) != nullptr);
}

PE_TEST(document_find_layer) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    PE_CHECK(doc->findLayer(base) != nullptr);
    PE_CHECK(doc->findLayer(kNoLayer) == nullptr);
    PE_CHECK(doc->findLayer(999999) == nullptr);
}

PE_TEST(document_blank_composites_transparent) {
    auto doc = Document::createBlank(Size{8, 8});
    PixelBuffer img = doc->compositeImage();
    PE_CHECK_EQ(img.width(), 8);
    PE_CHECK_EQ(img.at(0, 0), (Rgba8{0, 0, 0, 0}));
    PE_CHECK_EQ(img.at(7, 7), (Rgba8{0, 0, 0, 0}));
}

PE_TEST(document_set_active_layer) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->setActiveLayer(kNoLayer);  // clearing is allowed
    PE_CHECK_EQ(doc->activeLayer(), kNoLayer);
    doc->setActiveLayer(base);
    PE_CHECK_EQ(doc->activeLayer(), base);
    doc->setActiveLayer(424242);  // nonexistent -> ignored
    PE_CHECK_EQ(doc->activeLayer(), base);
}
