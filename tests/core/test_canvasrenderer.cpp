#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstdlib>
#include <memory>

using namespace pe;

namespace {
constexpr Rgba8 kRed{255, 0, 0, 255};
bool near8(Rgba8 a, Rgba8 b, int tol = 1) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol && d(a.a, b.a) <= tol;
}
}  // namespace

PE_TEST(renderer_first_render_composites_all_visible_tiles) {
    // 512x512 == a 2x2 tile grid.
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    PixelBuffer img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount(), static_cast<uint64_t>(4));
    PE_CHECK_EQ(img.at(0, 0), (Rgba8{0, 0, 0, 0}));  // empty base -> transparent
}

PE_TEST(renderer_recomposites_only_on_change) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());  // warm the cache (4)

    // Re-render with no change: zero recomposites (pure cache hit, like a pan).
    const uint64_t before = r.recompositeCount();
    (void)r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(0));
}

PE_TEST(renderer_invalidates_on_edit_and_reflects_it) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());  // cache 4 transparent tiles

    // Add a full-canvas red layer; the observer invalidates the affected tiles.
    auto layer = std::make_unique<SolidColorLayer>(kRed, doc->canvasBounds());
    const LayerId id = layer->id();
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));

    uint64_t before = r.recompositeCount();
    PixelBuffer img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(4));  // 4 dirty tiles
    PE_CHECK(near8(img.at(0, 0), kRed));
    PE_CHECK(near8(img.at(500, 500), kRed));

    // Opacity change dirties the layer's region -> recomposite those tiles only.
    before = r.recompositeCount();
    doc->history().push(std::make_unique<SetOpacityCommand>(id, 0.5f));
    img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(4));
    PE_CHECK(near8(img.at(0, 0), Rgba8{255, 0, 0, 128}));
}

PE_TEST(renderer_partial_invalidate_recomposites_one_tile) {
    auto doc = Document::createBlank(Size{512, 512});
    CanvasRenderer r(*doc);
    (void)r.renderRegion(doc->canvasBounds());  // warm 4

    const uint64_t before = r.recompositeCount();
    r.invalidate(Rect{0, 0, 10, 10});  // touches tile (0,0) only
    (void)r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(r.recompositeCount() - before, static_cast<uint64_t>(1));
}

PE_TEST(renderer_undo_restores_pixels) {
    auto doc = Document::createBlank(Size{256, 256});  // single tile
    CanvasRenderer r(*doc);
    auto layer = std::make_unique<SolidColorLayer>(kRed, doc->canvasBounds());
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));
    PixelBuffer img = r.renderRegion(doc->canvasBounds());
    PE_CHECK(near8(img.at(0, 0), kRed));

    doc->history().undo();  // remove the red layer
    img = r.renderRegion(doc->canvasBounds());
    PE_CHECK_EQ(img.at(0, 0), (Rgba8{0, 0, 0, 0}));  // back to transparent
}
