// Merge Down, Merge Visible and Flatten. A layered editor that cannot combine layers is stuck,
// and the thing a merge must never do is change the picture.

#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/BlendMode.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <memory>
#include <vector>

using namespace pe;

namespace {

// A document with `n` full-canvas layers, bottom-first, in the colours given.
std::unique_ptr<Document> stackOf(std::vector<Rgba8> colours, Size size = Size{32, 32}) {
    auto doc = Document::createBlank(size);
    // createBlank leaves one empty layer; make it the bottom of the stack.
    auto* first = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    first->tiles().fillRect(Rect{0, 0, size.width, size.height}, colours.front());
    for (std::size_t i = 1; i < colours.size(); ++i) {
        auto l = std::make_unique<PixelLayer>("L" + std::to_string(i));
        l->tiles().fillRect(Rect{0, 0, size.width, size.height}, colours[i]);
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(l));
    }
    return doc;
}

Layer* topLevel(Document& doc, std::size_t i) {
    return doc.topLevelLayers()[i].get();
}

Rgba8 pixelOf(const Layer* l, int x, int y) {
    return static_cast<const PixelLayer*>(l)->tiles().pixel(x, y);
}

// The composite as the canvas would draw it: what a merge has to preserve.
Rgba8 compositeAt(const Document& doc, int x, int y) {
    const PixelBuffer img = compositeToImage(doc.topLevelLayers(), doc.canvasBounds());
    return img.isEmpty() ? Rgba8{} : img.at(x, y);
}

bool near(int a, int b, int slack = 2) {
    return a - b <= slack && b - a <= slack;
}

}  // namespace

PE_TEST(merge_down_leaves_one_layer_holding_what_the_two_showed) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    const Rgba8 before = compositeAt(*doc, 8, 8);
    const std::vector<std::size_t> idx = mergeDownIndices(*doc, topLevel(*doc, 1)->id());
    PE_REQUIRE(idx.size() == 2);

    auto cmd = std::make_unique<MergeLayersCommand>(idx, "Merge Down", "L1");
    auto* raw = cmd.get();
    doc->history().push(std::move(cmd));

    PE_CHECK(raw->merged());
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    // The picture is unchanged, which is the whole contract of a merge.
    const Rgba8 after = compositeAt(*doc, 8, 8);
    PE_CHECK(near(after.r, before.r));
    PE_CHECK(near(after.g, before.g));
    PE_CHECK(near(after.b, before.b));
    PE_CHECK(near(after.a, before.a));
    PE_CHECK(pixelOf(topLevel(*doc, 0), 8, 8).b > 250);  // the top layer won, as it did before
}

PE_TEST(merge_bakes_opacity_and_blend_into_the_pixels_and_resets_them) {
    // A survivor left at 50% would apply the opacity a second time, and the picture would
    // change the moment it was merged.
    auto doc = stackOf({Rgba8{255, 255, 255, 255}, Rgba8{0, 0, 0, 255}});
    topLevel(*doc, 1)->setOpacity(0.5f);
    const Rgba8 before = compositeAt(*doc, 4, 4);
    PE_REQUIRE(near(before.r, 128, 3));  // half black over white

    doc->history().push(std::make_unique<MergeLayersCommand>(
        mergeDownIndices(*doc, topLevel(*doc, 1)->id()), "Merge Down", "Merged"));

    PE_REQUIRE(doc->topLevelCount() == 1);
    const Layer* survivor = topLevel(*doc, 0);
    PE_CHECK(near(pixelOf(survivor, 4, 4).r, 128, 3));  // the grey is in the pixels now
    PE_CHECK(survivor->opacity() > 0.999f);             // and not applied twice
    PE_CHECK(survivor->blendMode() == BlendMode::Normal);
    PE_CHECK(survivor->mask() == nullptr);
    PE_CHECK(!survivor->clipped());
    PE_CHECK(near(compositeAt(*doc, 4, 4).r, before.r, 3));
}

PE_TEST(merge_bakes_a_mask_into_the_pixels) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
    mask->buffer().fillRect(Rect{16, 0, 16, 32}, MaskBuffer::kClear);  // hide the right half
    topLevel(*doc, 1)->setMask(std::move(mask));
    const Rgba8 leftBefore = compositeAt(*doc, 4, 4);
    const Rgba8 rightBefore = compositeAt(*doc, 24, 4);
    PE_REQUIRE(leftBefore.b > 250);   // blue shows on the left
    PE_REQUIRE(rightBefore.r > 250);  // red shows through on the right

    doc->history().push(std::make_unique<MergeLayersCommand>(
        mergeDownIndices(*doc, topLevel(*doc, 1)->id()), "Merge Down", "Merged"));

    PE_REQUIRE(doc->topLevelCount() == 1);
    PE_CHECK(pixelOf(topLevel(*doc, 0), 4, 4).b > 250);
    PE_CHECK(pixelOf(topLevel(*doc, 0), 24, 4).r > 250);
    PE_CHECK(topLevel(*doc, 0)->mask() == nullptr);  // the mask is spent
}

PE_TEST(merge_bakes_an_adjustment_layer_into_what_is_below_it) {
    // Merging an adjustment layer down is how a non-destructive edit is made permanent.
    auto doc = stackOf({Rgba8{200, 100, 50, 255}});
    auto adj = std::make_unique<AdjustmentLayer>(std::make_unique<Invert>(), "Negative");
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));
    const Rgba8 before = compositeAt(*doc, 4, 4);
    PE_REQUIRE(near(before.r, 55, 3));  // 255 - 200

    doc->history().push(std::make_unique<MergeLayersCommand>(
        mergeDownIndices(*doc, topLevel(*doc, 1)->id()), "Merge Down", "Merged"));

    PE_REQUIRE(doc->topLevelCount() == 1);
    PE_CHECK(topLevel(*doc, 0)->kind() == LayerKind::Pixel);  // an adjustment merged to pixels
    PE_CHECK(near(pixelOf(topLevel(*doc, 0), 4, 4).r, 55, 3));
}

PE_TEST(merge_flattens_a_group_into_a_pixel_layer) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}});
    auto group = std::make_unique<GroupLayer>("Group");
    auto inner = std::make_unique<PixelLayer>("Inner");
    inner->tiles().fillRect(Rect{0, 0, 16, 32}, Rgba8{0, 255, 0, 255});
    group->addChild(std::move(inner));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
    const Rgba8 before = compositeAt(*doc, 4, 4);
    PE_REQUIRE(before.g > 250);

    doc->history().push(std::make_unique<MergeLayersCommand>(
        mergeDownIndices(*doc, topLevel(*doc, 1)->id()), "Merge Down", "Merged"));

    PE_REQUIRE(doc->topLevelCount() == 1);
    PE_CHECK(topLevel(*doc, 0)->kind() == LayerKind::Pixel);
    PE_CHECK(pixelOf(topLevel(*doc, 0), 4, 4).g > 250);   // the group's content came through
    PE_CHECK(pixelOf(topLevel(*doc, 0), 24, 4).r > 250);  // and the layer under it, where it did
}

PE_TEST(merge_undoes_back_to_the_layers_it_took) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    topLevel(*doc, 1)->setOpacity(0.25f);
    topLevel(*doc, 1)->setName("Top");
    const LayerId topId = topLevel(*doc, 1)->id();
    const LayerId bottomId = topLevel(*doc, 0)->id();

    doc->history().push(std::make_unique<MergeLayersCommand>(mergeDownIndices(*doc, topId),
                                                             "Merge Down", "Merged"));
    PE_REQUIRE(doc->topLevelCount() == 1);

    doc->history().undo();

    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    // Same layers, same ids, same order, same properties: nothing was rebuilt from the merge.
    PE_CHECK(topLevel(*doc, 0)->id() == bottomId);
    PE_CHECK(topLevel(*doc, 1)->id() == topId);
    PE_CHECK(topLevel(*doc, 1)->name() == "Top");
    PE_CHECK(near(static_cast<int>(topLevel(*doc, 1)->opacity() * 100.0f), 25));
    PE_CHECK(pixelOf(topLevel(*doc, 1), 4, 4).b > 250);
}

PE_TEST(merge_redoes_after_an_undo) {
    // The second execute must start from the restored originals rather than from the state the
    // first one left behind.
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    doc->history().push(std::make_unique<MergeLayersCommand>(
        mergeDownIndices(*doc, topLevel(*doc, 1)->id()), "Merge Down", "Merged"));
    doc->history().undo();
    PE_REQUIRE(doc->topLevelCount() == 2);
    doc->history().redo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK(pixelOf(topLevel(*doc, 0), 4, 4).b > 250);
}

PE_TEST(merge_visible_leaves_the_hidden_layers_where_they_are) {
    // Photoshop's rule: the result lands where the bottom-most visible layer was, and anything
    // hidden stays put rather than being merged or deleted.
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 255, 0, 255}, Rgba8{0, 0, 255, 255}});
    topLevel(*doc, 1)->setVisible(false);  // the green middle layer
    const LayerId hiddenId = topLevel(*doc, 1)->id();

    const std::vector<std::size_t> idx = mergeVisibleIndices(*doc);
    PE_REQUIRE(idx.size() == 2);
    PE_CHECK_EQ(idx[0], static_cast<std::size_t>(0));
    PE_CHECK_EQ(idx[1], static_cast<std::size_t>(2));

    doc->history().push(std::make_unique<MergeLayersCommand>(idx, "Merge Visible", "Merged"));

    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    PE_CHECK(doc->findLayer(hiddenId) != nullptr);  // still there
    PE_CHECK(!doc->findLayer(hiddenId)->visible());
    // The merged layer took the bottom slot, and the hidden one is above it now.
    PE_CHECK(topLevel(*doc, 0)->kind() == LayerKind::Pixel);
    PE_CHECK(pixelOf(topLevel(*doc, 0), 4, 4).b > 250);
    PE_CHECK(topLevel(*doc, 1)->id() == hiddenId);
}

PE_TEST(flatten_takes_everything_including_the_hidden_layers) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 255, 0, 255}, Rgba8{0, 0, 255, 255}});
    topLevel(*doc, 1)->setVisible(false);

    const std::vector<std::size_t> idx = flattenIndices(*doc);
    PE_REQUIRE(idx.size() == 3);
    doc->history().push(std::make_unique<MergeLayersCommand>(idx, "Flatten Image", "Background"));

    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK(topLevel(*doc, 0)->name() == "Background");
    // A hidden layer contributes nothing, so flattening discards it rather than revealing it.
    PE_CHECK(pixelOf(topLevel(*doc, 0), 4, 4).b > 250);
    PE_CHECK(pixelOf(topLevel(*doc, 0), 4, 4).g < 5);
}

PE_TEST(merge_refuses_when_the_lowest_layer_is_clipped) {
    // Its clipping base sits below the set, so merging would render it unclipped and the
    // picture would change. A merge that changes the picture is worse than one that declines.
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 255, 0, 255}, Rgba8{0, 0, 255, 255}});
    topLevel(*doc, 1)->setClipped(true);

    auto cmd = std::make_unique<MergeLayersCommand>(std::vector<std::size_t>{1, 2}, "Merge Down",
                                                    "Merged");
    auto* raw = cmd.get();
    doc->history().push(std::move(cmd));

    PE_CHECK(!raw->merged());
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(3));  // nothing happened
}

PE_TEST(merge_of_a_single_layer_does_nothing) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}});
    auto cmd = std::make_unique<MergeLayersCommand>(std::vector<std::size_t>{0}, "Merge", "M");
    auto* raw = cmd.get();
    doc->history().push(std::move(cmd));
    PE_CHECK(!raw->merged());
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
}

PE_TEST(merge_modes_report_nothing_to_do_rather_than_a_bad_run) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}});
    // Nothing under the bottom layer to merge into.
    PE_CHECK(mergeDownIndices(*doc, topLevel(*doc, 0)->id()).empty());
    // A layer that is not in this document at all.
    PE_CHECK(mergeDownIndices(*doc, kNoLayer).empty());
    // One layer is not a merge, in either mode.
    PE_CHECK(mergeVisibleIndices(*doc).empty());
    PE_CHECK(flattenIndices(*doc).empty());

    // Two layers with only one visible is not a Merge Visible either.
    auto two = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    topLevel(*two, 1)->setVisible(false);
    PE_CHECK(mergeVisibleIndices(*two).empty());
    PE_CHECK(flattenIndices(*two).size() == 2);  // flatten still has something to do
}

PE_TEST(merge_of_a_sparse_result_does_not_fill_the_canvas_with_tiles) {
    // A merge writes the canvas-sized composite. Storing its transparent parts would turn a
    // small drawing into a fully materialized canvas on every merge.
    auto doc = Document::createBlank(Size{1024, 1024});
    auto* first = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    first->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{255, 0, 0, 255});
    auto second = std::make_unique<PixelLayer>("Second");
    second->tiles().fillRect(Rect{8, 0, 8, 8}, Rgba8{0, 0, 255, 255});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(second));

    doc->history().push(
        std::make_unique<MergeLayersCommand>(flattenIndices(*doc), "Flatten", "Background"));
    PE_REQUIRE(doc->topLevelCount() == 1);
    // 1024x1024 is 16 tiles; the content fits in one.
    PE_CHECK_EQ(static_cast<const PixelLayer*>(topLevel(*doc, 0))->tiles().tileCount(),
                static_cast<std::size_t>(1));
}

PE_TEST(merge_makes_the_survivor_the_active_layer) {
    // The layers the active one was may be gone. Leaving the active id dangling, or pointing at
    // something the user did not choose, is how the next edit lands somewhere surprising.
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    doc->setActiveLayer(topLevel(*doc, 1)->id());
    doc->history().push(std::make_unique<MergeLayersCommand>(
        mergeDownIndices(*doc, doc->activeLayer()), "Merge Down", "Merged"));
    PE_REQUIRE(doc->topLevelCount() == 1);
    PE_CHECK(doc->activeLayer() == topLevel(*doc, 0)->id());
    PE_CHECK(doc->findLayer(doc->activeLayer()) != nullptr);
}

PE_TEST(merge_of_a_canvas_over_the_composite_cap_is_blocked_rather_than_attempted) {
    // compositeToImage returns an empty buffer past the cap. A merge that went ahead anyway
    // would replace every layer it took with a blank one, which is the worst possible outcome
    // for an operation whose whole job is to preserve the picture.
    auto doc = Document::createBlank(Size{9000, 9000});  // 81 MP, over the 64 MP cap
    PE_REQUIRE(doc != nullptr);
    auto second = std::make_unique<PixelLayer>("Second");
    second->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{0, 0, 255, 255});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(second));

    const std::vector<std::size_t> idx = flattenIndices(*doc);
    PE_REQUIRE(idx.size() == 2);
    PE_CHECK(mergeBlocker(*doc, idx) == MergeBlock::OverCompositeCap);

    auto cmd = std::make_unique<MergeLayersCommand>(idx, "Flatten", "Background");
    auto* raw = cmd.get();
    doc->history().push(std::move(cmd));
    PE_CHECK(!raw->merged());
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));  // both layers still there
}

PE_TEST(merge_blocker_names_which_guard_stopped_it) {
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 255, 0, 255}, Rgba8{0, 0, 255, 255}});
    PE_CHECK(mergeBlocker(*doc, std::vector<std::size_t>{0, 1}) == MergeBlock::None);
    PE_CHECK(mergeBlocker(*doc, std::vector<std::size_t>{1}) == MergeBlock::TooFewLayers);
    PE_CHECK(mergeBlocker(*doc, std::vector<std::size_t>{0, 99}) == MergeBlock::TooFewLayers);
    topLevel(*doc, 1)->setClipped(true);
    PE_CHECK(mergeBlocker(*doc, std::vector<std::size_t>{1, 2}) == MergeBlock::LowestIsClipped);
    // A clipped layer that is NOT the lowest is fine: its base is inside the set.
    PE_CHECK(mergeBlocker(*doc, std::vector<std::size_t>{0, 1, 2}) == MergeBlock::None);
}

PE_TEST(merge_executing_again_without_an_undo_starts_over_rather_than_stacking_state) {
    // History always undoes before it redoes, so this never happens through the normal path.
    // It is still the command's contract: merged() has to describe the LAST run, and a second
    // execute must not keep the first one's removed layers alive, or a refused re-run would
    // report success and hold a set of layers nothing can put back.
    auto doc = stackOf({Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}});
    MergeLayersCommand cmd(mergeDownIndices(*doc, topLevel(*doc, 1)->id()), "Merge Down", "M");

    (void)cmd.execute(*doc);
    PE_REQUIRE(cmd.merged());
    PE_REQUIRE(doc->topLevelCount() == 1);

    // One layer left, so this run has nothing to merge and must say so.
    (void)cmd.execute(*doc);
    PE_CHECK(!cmd.merged());
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
}
