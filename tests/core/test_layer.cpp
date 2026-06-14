#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <string>
#include <vector>

using namespace pe;

PE_TEST(layer_defaults_and_ids) {
    PixelLayer a("A");
    PixelLayer b("B");
    PE_CHECK(a.id() != kNoLayer);
    PE_CHECK(a.id() != b.id());  // unique
    PE_CHECK(a.kind() == LayerKind::Pixel);
    PE_CHECK_EQ(a.name(), std::string("A"));
    PE_CHECK(a.visible());
    PE_CHECK_NEAR(a.opacity(), 1.0f);
    PE_CHECK(a.blendMode() == BlendMode::Normal);
    PE_CHECK(!a.clipped());
}

PE_TEST(layer_opacity_clamped) {
    PixelLayer a("A");
    a.setOpacity(2.0f);
    PE_CHECK_NEAR(a.opacity(), 1.0f);
    a.setOpacity(-0.5f);
    PE_CHECK_NEAR(a.opacity(), 0.0f);
    a.setFillOpacity(0.25f);
    PE_CHECK_NEAR(a.fillOpacity(), 0.25f);
}

PE_TEST(layer_clone_fresh_identity_same_props) {
    SolidColorLayer s(Rgba8{10, 20, 30, 255}, Rect{0, 0, 5, 5}, "Fill");
    s.setOpacity(0.5f);
    s.setBlendMode(BlendMode::Multiply);
    s.setVisible(false);

    auto c = s.clone();
    PE_CHECK(c->id() != s.id());  // fresh identity
    PE_CHECK_EQ(c->name(), std::string("Fill"));
    PE_CHECK_NEAR(c->opacity(), 0.5f);
    PE_CHECK(c->blendMode() == BlendMode::Multiply);
    PE_CHECK(!c->visible());
    PE_CHECK(c->kind() == LayerKind::Fill);
}

PE_TEST(layer_kind_names) {
    PE_CHECK_EQ(std::string(layerKindName(LayerKind::Pixel)), std::string("Pixel"));
    PE_CHECK_EQ(std::string(layerKindName(LayerKind::Group)), std::string("Group"));
}

PE_TEST(group_child_management) {
    GroupLayer g("g");
    PE_CHECK_EQ(g.childCount(), static_cast<std::size_t>(0));
    auto child = std::make_unique<PixelLayer>("c1");
    const LayerId cid = child->id();
    g.addChild(std::move(child));
    PE_CHECK_EQ(g.childCount(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(g.indexOf(cid), static_cast<std::size_t>(0));
    PE_CHECK(g.findChild(cid) != nullptr);
    PE_CHECK(g.findDescendant(cid) != nullptr);

    auto removed = g.removeChild(cid);
    PE_CHECK(removed != nullptr);
    PE_CHECK_EQ(g.childCount(), static_cast<std::size_t>(0));
    PE_CHECK(g.removeChild(cid) == nullptr);  // already gone
}

PE_TEST(group_clone_is_deep_with_fresh_ids) {
    auto g = std::make_unique<GroupLayer>("g");
    auto child = std::make_unique<PixelLayer>("c");
    const LayerId childId = child->id();
    g->addChild(std::move(child));

    auto clone = g->clone();
    PE_CHECK(clone->id() != g->id());
    auto* gc = static_cast<GroupLayer*>(clone.get());
    PE_CHECK_EQ(gc->childCount(), static_cast<std::size_t>(1));
    // The cloned child has a fresh id (deep copy, not a shared pointer).
    PE_CHECK(gc->findDescendant(childId) == nullptr);
}

PE_TEST(pixellayer_16bit_storage_renders_full_precision) {
    // A 16-bit layer carries values with no 8-bit equivalent end-to-end: stored in
    // tiles16(), renderInto converts them to float losslessly (no 8-bit round-trip).
    PixelLayer layer("Deep", BitDepth::U16);
    PE_CHECK(layer.depth() == BitDepth::U16);

    const Rgba16 deep{40000, 257, 65535, 65535};  // 40000 is not on the 8-bit grid
    layer.tiles16().setPixel(3, 4, deep);
    PE_CHECK(layer.hasTileAt(TileCoord{0, 0}));
    PE_CHECK_EQ(layer.contentBounds(), tileBounds(TileCoord{0, 0}));

    std::vector<Rgbaf> tile(static_cast<std::size_t>(kTilePixels));
    layer.renderInto(TileCoord{0, 0}, tile);
    const std::size_t idx = static_cast<std::size_t>(4) * kTileSize + 3;
    PE_CHECK_NEAR(tile[idx].r, 40000.0f / 65535.0f);
    PE_CHECK_NEAR(tile[idx].g, 257.0f / 65535.0f);
    PE_CHECK_NEAR(tile[idx].a, 1.0f);
    PE_CHECK_NEAR(tile[0].a, 0.0f);  // untouched pixel transparent
}

PE_TEST(pixellayer_float_storage_preserves_hdr) {
    PixelLayer layer("HDR", BitDepth::F32);
    PE_CHECK(layer.depth() == BitDepth::F32);
    layer.tilesF().setPixel(1, 1, Rgbaf{2.5f, 0.25f, -0.1f, 1.0f});  // out-of-range
    std::vector<Rgbaf> tile(static_cast<std::size_t>(kTilePixels));
    layer.renderInto(TileCoord{0, 0}, tile);
    const std::size_t idx = static_cast<std::size_t>(1) * kTileSize + 1;
    PE_CHECK_NEAR(tile[idx].r, 2.5f);   // HDR value passes through
    PE_CHECK_NEAR(tile[idx].b, -0.1f);  // negative preserved
}

PE_TEST(pixellayer_clone_preserves_depth_and_content) {
    PixelLayer layer("Deep", BitDepth::U16);
    const Rgba16 deep{12345, 0, 0, 65535};
    layer.tiles16().setPixel(2, 2, deep);
    auto copy = layer.clone();
    auto* pc = static_cast<PixelLayer*>(copy.get());
    PE_CHECK(pc->depth() == BitDepth::U16);
    PE_CHECK_EQ(pc->tiles16().pixel(2, 2), deep);
    // Shared copy-on-write: editing the clone does not touch the original.
    pc->tiles16().setPixel(2, 2, Rgba16{1, 1, 1, 1});
    PE_CHECK_EQ(layer.tiles16().pixel(2, 2), deep);
}

PE_TEST(pixellayer_default_is_8bit_unchanged) {
    PixelLayer layer("Plain");
    PE_CHECK(layer.depth() == BitDepth::U8);
    layer.tiles().setPixel(0, 0, Rgba8{10, 20, 30, 255});
    std::vector<Rgbaf> tile(static_cast<std::size_t>(kTilePixels));
    layer.renderInto(TileCoord{0, 0}, tile);
    PE_CHECK_NEAR(tile[0].r, 10.0f / 255.0f);
}
