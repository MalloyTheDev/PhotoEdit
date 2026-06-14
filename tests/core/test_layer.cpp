#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <string>

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
