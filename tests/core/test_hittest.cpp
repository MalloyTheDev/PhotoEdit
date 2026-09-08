// The Move tool's Auto-Select needs to know which layer is under a document point, and
// nothing in the engine could answer that: the compositor flattens the stack and throws
// the identity away. These pin the rule, and in particular the cases that decide whether
// Auto-Select feels correct or arbitrary: a click has to fall THROUGH anything the user
// cannot see, and stop at the first thing they can.

#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/HitTest.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace {

// A pixel layer with an opaque square at `r`, and nothing anywhere else.
std::unique_ptr<pe::PixelLayer> square(const std::string& name, pe::Rect r, pe::Rgba8 c) {
    auto l = std::make_unique<pe::PixelLayer>(name);
    l->tiles().fillRect(r, c);
    return l;
}

pe::LayerId add(pe::Document& doc, std::unique_ptr<pe::Layer> l) {
    const pe::LayerId id = l->id();
    doc.cmdInsertTopLevel(doc.topLevelCount(), std::move(l));
    return id;
}

constexpr pe::Rgba8 kRed{200, 40, 40, 255};
constexpr pe::Rgba8 kBlue{40, 40, 200, 255};

}  // namespace

PE_TEST(hittest_finds_nothing_in_an_empty_document) {
    // createBlank starts with one fully transparent layer, so every point misses. An
    // empty document must be safe to click, not a special case the caller has to guard.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    PE_CHECK(pe::layerAt(*doc, pe::Point{32, 32}) == pe::kNoLayer);
    PE_CHECK(pe::layerAt(*doc, pe::Point{0, 0}) == pe::kNoLayer);
    // And a point far outside the canvas, which no layer's bounds contain.
    PE_CHECK(pe::layerAt(*doc, pe::Point{100000, -100000}) == pe::kNoLayer);
}

PE_TEST(hittest_returns_the_topmost_layer_covering_the_point) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId low = add(*doc, square("Low", pe::Rect{0, 0, 40, 40}, kRed));
    const pe::LayerId high = add(*doc, square("High", pe::Rect{20, 20, 40, 40}, kBlue));

    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == high);         // both cover it; the top wins
    PE_CHECK(pe::layerAt(*doc, pe::Point{5, 5}) == low);            // only the lower one
    PE_CHECK(pe::layerAt(*doc, pe::Point{55, 55}) == high);         // only the upper one
    PE_CHECK(pe::layerAt(*doc, pe::Point{62, 5}) == pe::kNoLayer);  // neither
}

PE_TEST(hittest_falls_through_anything_the_user_cannot_see) {
    // Four separate ways for a layer to be present in the stack and invisible on screen.
    // Each has to be transparent to a click, or Auto-Select grabs something the user has
    // no reason to believe is there.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId below = add(*doc, square("Below", pe::Rect{0, 0, 64, 64}, kRed));

    // 1. A transparent pixel inside an otherwise painted layer.
    auto holed = square("Holed", pe::Rect{0, 0, 64, 64}, kBlue);
    holed->tiles().fillRect(pe::Rect{10, 10, 8, 8}, pe::Rgba8{0, 0, 0, 0});
    const pe::LayerId holedId = add(*doc, std::move(holed));
    PE_CHECK(pe::layerAt(*doc, pe::Point{14, 14}) == below);
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == holedId);

    // 2. Hidden.
    doc->findLayer(holedId)->setVisible(false);
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == below);
    doc->findLayer(holedId)->setVisible(true);

    // 3. Zero opacity.
    doc->findLayer(holedId)->setOpacity(0.0f);
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == below);
    doc->findLayer(holedId)->setOpacity(1.0f);

    // 4. Masked out. The mask reveals nothing at (30,30) and everything at (50,50).
    auto mask = std::make_unique<pe::Mask>();
    mask->buffer().fillRect(pe::Rect{0, 0, 64, 64}, 255);
    mask->buffer().fillRect(pe::Rect{24, 24, 12, 12}, 0);
    doc->findLayer(holedId)->setMask(std::move(mask));
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == below);
    PE_CHECK(pe::layerAt(*doc, pe::Point{50, 50}) == holedId);

    // A disabled mask hides nothing, so the layer is back.
    doc->findLayer(holedId)->mask()->setEnabled(false);
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == holedId);
}

PE_TEST(hittest_skips_adjustment_layers) {
    // An adjustment has no pixels of its own, and AdjustmentLayer::contentBounds() is the
    // whole representable plane, so without an explicit skip it would answer every click
    // in the document, including clicks on empty canvas.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId painted = add(*doc, square("Painted", pe::Rect{0, 0, 32, 32}, kRed));
    add(*doc, std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::BrightnessContrast>(),
                                                    "Brightness"));

    PE_CHECK(pe::layerAt(*doc, pe::Point{10, 10}) == painted);
    PE_CHECK(pe::layerAt(*doc, pe::Point{50, 50}) == pe::kNoLayer);  // not the adjustment

    // The skip is also the whole point of the cost, and only the counter can see it: an
    // adjustment layer's content bounds are the whole plane, so they reject nothing, and
    // without the skip every click would render a megabyte tile per adjustment layer to
    // read one transparent pixel. The answer is identical either way.
    add(*doc, std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::Levels>(), "Levels"));
    const std::uint64_t before = pe::hitTestTileRenderCount();
    PE_CHECK(pe::layerAt(*doc, pe::Point{50, 50}) == pe::kNoLayer);
    PE_CHECK(pe::layerAt(*doc, pe::Point{10, 10}) == painted);
    // Two adjustment layers skipped, and the raster layers read straight out of their tile
    // stores: a whole document walked twice without rendering a single tile.
    PE_CHECK_EQ(pe::hitTestTileRenderCount(), before);
}

PE_TEST(hittest_descends_into_groups_and_reports_the_child) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId loose = add(*doc, square("Loose", pe::Rect{0, 0, 64, 64}, kRed));

    auto group = std::make_unique<pe::GroupLayer>("G");
    group->addChild(square("Inner", pe::Rect{16, 16, 16, 16}, kBlue));
    const pe::LayerId inner = group->children()[0]->id();
    const pe::LayerId groupId = add(*doc, std::move(group));

    // The layer under the point is the child, not the group that holds it.
    PE_CHECK(pe::layerAt(*doc, pe::Point{20, 20}) == inner);
    // Group granularity is that answer walked up to the top level.
    PE_CHECK(pe::topLevelAncestorOf(*doc, inner) == groupId);
    // A top-level layer is its own top-level ancestor.
    PE_CHECK(pe::topLevelAncestorOf(*doc, loose) == loose);
    PE_CHECK(pe::topLevelAncestorOf(*doc, groupId) == groupId);
    // An id the document does not hold has no ancestor, and neither does kNoLayer.
    PE_CHECK(pe::topLevelAncestorOf(*doc, 999999) == pe::kNoLayer);
    PE_CHECK(pe::topLevelAncestorOf(*doc, pe::kNoLayer) == pe::kNoLayer);

    // Outside the child, the group contributes nothing and the click reaches what is
    // behind it.
    PE_CHECK(pe::layerAt(*doc, pe::Point{50, 50}) == loose);
}

PE_TEST(hittest_a_hidden_or_masked_group_hides_its_children) {
    // A group's visibility, opacity and mask apply to everything inside it. A click on a
    // child the group has hidden must fall through to what is behind the GROUP, not
    // select a layer that contributes no pixels to the screen.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId below = add(*doc, square("Below", pe::Rect{0, 0, 64, 64}, kRed));

    auto group = std::make_unique<pe::GroupLayer>("G");
    group->addChild(square("Inner", pe::Rect{16, 16, 32, 32}, kBlue));
    const pe::LayerId inner = group->children()[0]->id();
    const pe::LayerId groupId = add(*doc, std::move(group));
    PE_REQUIRE(pe::layerAt(*doc, pe::Point{20, 20}) == inner);

    doc->findLayer(groupId)->setVisible(false);
    PE_CHECK(pe::layerAt(*doc, pe::Point{20, 20}) == below);
    doc->findLayer(groupId)->setVisible(true);

    doc->findLayer(groupId)->setOpacity(0.0f);
    PE_CHECK(pe::layerAt(*doc, pe::Point{20, 20}) == below);
    doc->findLayer(groupId)->setOpacity(1.0f);

    auto mask = std::make_unique<pe::Mask>();
    mask->buffer().fillRect(pe::Rect{0, 0, 64, 64}, 255);
    mask->buffer().fillRect(pe::Rect{16, 16, 12, 12}, 0);
    doc->findLayer(groupId)->setMask(std::move(mask));
    PE_CHECK(pe::layerAt(*doc, pe::Point{20, 20}) == below);  // masked out of the group
    PE_CHECK(pe::layerAt(*doc, pe::Point{40, 40}) == inner);  // still revealed here
}

PE_TEST(hittest_respects_the_threshold_and_works_off_canvas) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId faint =
        add(*doc, square("Faint", pe::Rect{0, 0, 64, 64}, pe::Rgba8{10, 10, 10, 20}));

    // The default threshold rejects only what is exactly transparent, so a barely visible
    // layer is still grabbable, which is what a user dragging an almost-clear layer wants.
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}) == faint);
    // Raised above its alpha (20/255 is about 0.078), it stops counting.
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}, 0.5f) == pe::kNoLayer);
    // A negative threshold cannot make a transparent pixel a hit.
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}, -1.0f) == faint);

    // A group at partial opacity dims everything inside it, so a child that would clear the
    // threshold on its own may not once the group is applied. The numbers are chosen so the
    // GROUP still clears the threshold and is descended into: otherwise the group is
    // rejected whole and the dimming of the child is never exercised at all.
    auto dim = std::make_unique<pe::GroupLayer>("Dim");
    dim->addChild(square("Half", pe::Rect{0, 0, 64, 64}, pe::Rgba8{255, 255, 255, 128}));
    const pe::LayerId half = dim->children()[0]->id();
    dim->setOpacity(0.5f);
    add(*doc, std::move(dim));
    // The child alone is 128/255, about 0.50; through the group it is about 0.25.
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}, 0.2f) == half);          // 0.25 clears 0.2
    PE_CHECK(pe::layerAt(*doc, pe::Point{30, 30}, 0.4f) == pe::kNoLayer);  // but not 0.4,
    // which the group's own 0.5 gate does clear, so the descent happened either way.

    // Layer content may sit at negative document coordinates, where the tile index is a
    // floor division rather than a truncation. Getting that wrong reads the wrong tile.
    const pe::LayerId offCanvas = add(*doc, square("Off", pe::Rect{-300, -300, 100, 100}, kBlue));
    PE_CHECK(pe::layerAt(*doc, pe::Point{-250, -250}) == offCanvas);
    PE_CHECK(pe::layerAt(*doc, pe::Point{-150, -150}) == pe::kNoLayer);
}

PE_TEST(hittest_handles_a_non_pixel_layer) {
    // The walk goes through Layer::renderInto rather than reaching into a PixelLayer's
    // tiles, so a layer kind with no tile store at all still answers correctly.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId fill =
        add(*doc, std::make_unique<pe::SolidColorLayer>(kRed, pe::Rect{8, 8, 16, 16}, "Fill"));

    const std::uint64_t before = pe::hitTestTileRenderCount();
    PE_CHECK(pe::layerAt(*doc, pe::Point{12, 12}) == fill);
    PE_CHECK(pe::layerAt(*doc, pe::Point{40, 40}) == pe::kNoLayer);
    // This is the kind that HAS to be rendered, since it keeps no tiles to read: once for
    // the hit, and not at all for the miss, which its bounds reject.
    PE_CHECK_EQ(pe::hitTestTileRenderCount(), before + 1);
}
