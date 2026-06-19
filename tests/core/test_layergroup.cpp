#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <memory>
#include <vector>

using namespace pe;

namespace {

const Rect kCanvas{0, 0, 16, 16};
constexpr Rgba8 kRed{255, 0, 0, 255};

std::unique_ptr<Layer> fill() {
    return std::make_unique<SolidColorLayer>(kRed, kCanvas);
}

// Add `n` top-level fill layers and return their ids bottom-to-top (in push order).
std::vector<LayerId> addLayers(Document& doc, int n) {
    std::vector<LayerId> ids;
    for (int i = 0; i < n; ++i) {
        auto layer = fill();
        ids.push_back(layer->id());
        doc.history().push(
            std::make_unique<AddLayerCommand>(std::move(layer), doc.topLevelCount()));
    }
    return ids;
}

const GroupLayer* groupAt(const Document& doc, std::size_t idx) {
    const auto layers = doc.topLevelLayers();
    if (idx >= layers.size() || layers[idx]->kind() != LayerKind::Group) return nullptr;
    return static_cast<const GroupLayer*>(layers[idx].get());
}

}  // namespace

PE_TEST(group_three_layers_into_group_in_order) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();              // index 0
    const std::vector<LayerId> ids = addLayers(*doc, 3);  // indices 1,2,3
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(4));

    doc->history().push(std::make_unique<GroupLayersCommand>(ids));

    // base remains at 0; a new group replaces the three layers at index 1.
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
    const GroupLayer* grp = groupAt(*doc, 1);
    PE_CHECK(grp != nullptr);
    if (grp != nullptr) {
        PE_CHECK_EQ(grp->childCount(), static_cast<std::size_t>(3));
        // Children preserve the original relative order (bottom-to-top == push order).
        PE_CHECK_EQ(grp->children()[0]->id(), ids[0]);
        PE_CHECK_EQ(grp->children()[1]->id(), ids[1]);
        PE_CHECK_EQ(grp->children()[2]->id(), ids[2]);
        // The group is active and the members are still findable (nested).
        PE_CHECK_EQ(doc->activeLayer(), grp->id());
    }
    for (LayerId id : ids) PE_CHECK(doc->findLayer(id) != nullptr);
}

PE_TEST(group_undo_restores_flat_order_same_ids) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    const std::vector<LayerId> ids = addLayers(*doc, 3);

    doc->history().push(std::make_unique<GroupLayersCommand>(ids));
    doc->history().undo();

    // Exact pre-group layout restored: base, ids[0], ids[1], ids[2] at 0..3 with same ids.
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(4));
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[0]), static_cast<std::size_t>(1));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[1]), static_cast<std::size_t>(2));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[2]), static_cast<std::size_t>(3));
    // No stray group remains.
    for (std::size_t i = 0; i < doc->topLevelCount(); ++i)
        PE_CHECK(doc->topLevelLayers()[i]->kind() != LayerKind::Group);
    // Active layer restored to what it was before grouping. AddLayerCommand does not change
    // the active layer, so the base layer was active throughout — grouping made the group
    // active, and undo restores the base.
    PE_CHECK_EQ(doc->activeLayer(), base);
}

PE_TEST(group_redo_keeps_group_id_stable) {
    auto doc = Document::createBlank(Size{16, 16});
    const std::vector<LayerId> ids = addLayers(*doc, 2);  // base(0), ids[0](1), ids[1](2)

    doc->history().push(std::make_unique<GroupLayersCommand>(ids));
    const LayerId groupId = doc->activeLayer();
    PE_CHECK(groupId != kNoLayer);

    doc->history().undo();
    PE_CHECK(doc->findLayer(groupId) == nullptr);  // group gone while undone

    doc->history().redo();
    // Same group id reappears (the shell is reused, not re-created), children intact in order.
    // The group sits at index 1 (topmost member's original slot), above the base layer.
    const GroupLayer* grp = groupAt(*doc, 1);
    PE_CHECK(grp != nullptr);
    if (grp != nullptr) {
        PE_CHECK_EQ(grp->id(), groupId);
        PE_CHECK_EQ(grp->childCount(), static_cast<std::size_t>(2));
        PE_CHECK_EQ(grp->children()[0]->id(), ids[0]);
        PE_CHECK_EQ(grp->children()[1]->id(), ids[1]);
    }
    PE_CHECK_EQ(doc->activeLayer(), groupId);
}

PE_TEST(group_preserves_order_when_topmost_grouped_partially) {
    // Group a non-contiguous subset to verify relative order and insert position.
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();              // 0
    const std::vector<LayerId> ids = addLayers(*doc, 4);  // 1,2,3,4
    // Group ids[0] (idx1) and ids[2] (idx3), leaving ids[1] (idx2) and ids[3] (idx4) flat.
    doc->history().push(std::make_unique<GroupLayersCommand>(std::vector<LayerId>{ids[2], ids[0]}));

    // Group inserts at the topmost member's original index (1). Remaining: base(0), group(1),
    // ids[1], ids[3].
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(4));
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
    const GroupLayer* grp = groupAt(*doc, 1);
    PE_CHECK(grp != nullptr);
    if (grp != nullptr) {
        PE_CHECK_EQ(grp->childCount(), static_cast<std::size_t>(2));
        // Sorted by original index regardless of request order: ids[0] (idx1) then ids[2] (idx3).
        PE_CHECK_EQ(grp->children()[0]->id(), ids[0]);
        PE_CHECK_EQ(grp->children()[1]->id(), ids[2]);
    }
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[1]), static_cast<std::size_t>(2));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[3]), static_cast<std::size_t>(3));

    // Undo restores the exact original flat order.
    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(5));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[0]), static_cast<std::size_t>(1));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[1]), static_cast<std::size_t>(2));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[2]), static_cast<std::size_t>(3));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[3]), static_cast<std::size_t>(4));
}

PE_TEST(ungroup_splices_children_at_group_position) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    const std::vector<LayerId> ids = addLayers(*doc, 3);
    doc->history().push(std::make_unique<GroupLayersCommand>(ids));
    const LayerId groupId = doc->activeLayer();

    doc->history().push(std::make_unique<UngroupCommand>(groupId));

    // Group dissolved; children land back at the group's slot (1..3), base stays at 0.
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(4));
    PE_CHECK(doc->findLayer(groupId) == nullptr);
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[0]), static_cast<std::size_t>(1));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[1]), static_cast<std::size_t>(2));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[2]), static_cast<std::size_t>(3));
    // Active was the group, which is gone -> cleared.
    PE_CHECK_EQ(doc->activeLayer(), kNoLayer);
}

PE_TEST(ungroup_undo_reconstructs_group) {
    auto doc = Document::createBlank(Size{16, 16});
    const std::vector<LayerId> ids = addLayers(*doc, 3);
    doc->history().push(std::make_unique<GroupLayersCommand>(ids));
    const LayerId groupId = doc->activeLayer();
    doc->history().push(std::make_unique<UngroupCommand>(groupId));

    doc->history().undo();  // undo the ungroup

    // Same group id, same children in the same order, group active again.
    const GroupLayer* grp = groupAt(*doc, 1);
    PE_CHECK(grp != nullptr);
    if (grp != nullptr) {
        PE_CHECK_EQ(grp->id(), groupId);
        PE_CHECK_EQ(grp->childCount(), static_cast<std::size_t>(3));
        PE_CHECK_EQ(grp->children()[0]->id(), ids[0]);
        PE_CHECK_EQ(grp->children()[1]->id(), ids[1]);
        PE_CHECK_EQ(grp->children()[2]->id(), ids[2]);
    }
    PE_CHECK_EQ(doc->activeLayer(), groupId);

    // Redo dissolves again.
    doc->history().redo();
    PE_CHECK(doc->findLayer(groupId) == nullptr);
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[0]), static_cast<std::size_t>(1));
}

PE_TEST(group_empty_input_is_noop) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    const std::size_t before = doc->topLevelCount();

    doc->history().push(std::make_unique<GroupLayersCommand>(std::vector<LayerId>{}));
    PE_CHECK_EQ(doc->topLevelCount(), before);
    PE_CHECK_EQ(doc->activeLayer(), base);
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));

    // Undo of the no-op leaves the tree unchanged.
    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelCount(), before);
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
}

PE_TEST(group_invalid_input_is_noop) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    const std::vector<LayerId> ids = addLayers(*doc, 2);
    const std::size_t before = doc->topLevelCount();

    // Unknown id mixed in -> reject whole command.
    doc->history().push(std::make_unique<GroupLayersCommand>(std::vector<LayerId>{ids[0], 999999}));
    PE_CHECK_EQ(doc->topLevelCount(), before);
    for (LayerId id : ids) PE_CHECK(doc->topLevelIndexOf(id) != GroupLayer::npos);

    // Duplicate id -> reject.
    doc->history().push(std::make_unique<GroupLayersCommand>(std::vector<LayerId>{ids[1], ids[1]}));
    PE_CHECK_EQ(doc->topLevelCount(), before);

    // A nested id (not a top-level sibling) -> reject. First make a real group.
    doc->history().push(std::make_unique<GroupLayersCommand>(ids));
    const GroupLayer* grp = groupAt(*doc, 1);
    PE_CHECK(grp != nullptr);
    if (grp != nullptr) {
        const LayerId nested = grp->children()[0]->id();
        const std::size_t afterGroup = doc->topLevelCount();
        doc->history().push(
            std::make_unique<GroupLayersCommand>(std::vector<LayerId>{base, nested}));
        PE_CHECK_EQ(doc->topLevelCount(), afterGroup);  // unchanged: nested id rejected
    }
}

PE_TEST(ungroup_non_group_is_noop) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();  // a pixel layer, not a group
    const std::size_t before = doc->topLevelCount();

    doc->history().push(std::make_unique<UngroupCommand>(base));
    PE_CHECK_EQ(doc->topLevelCount(), before);
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));

    // Unknown id -> no-op.
    doc->history().push(std::make_unique<UngroupCommand>(424242));
    PE_CHECK_EQ(doc->topLevelCount(), before);

    doc->history().undo();  // undoing a no-op is harmless
    PE_CHECK_EQ(doc->topLevelCount(), before);
}

PE_TEST(group_then_ungroup_roundtrip_pixels_intact) {
    // End-to-end: group, ungroup, and the layer objects (their ids) survive intact.
    auto doc = Document::createBlank(Size{16, 16});
    const std::vector<LayerId> ids = addLayers(*doc, 2);
    Layer* before0 = doc->findLayer(ids[0]);
    PE_CHECK(before0 != nullptr);

    doc->history().push(std::make_unique<GroupLayersCommand>(ids));
    const LayerId groupId = doc->activeLayer();
    doc->history().push(std::make_unique<UngroupCommand>(groupId));

    // Same Layer* identity is not guaranteed by the API, but ids must be intact & top-level.
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[0]), static_cast<std::size_t>(1));
    PE_CHECK_EQ(doc->topLevelIndexOf(ids[1]), static_cast<std::size_t>(2));
    PE_CHECK(doc->findLayer(groupId) == nullptr);
}
