#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace pe;

namespace {

const Rect kCanvas{0, 0, 16, 16};
constexpr Rgba8 kRed{255, 0, 0, 255};

std::unique_ptr<Layer> redFill() {
    return std::make_unique<SolidColorLayer>(kRed, kCanvas);
}

// Records the change kinds it observes, for asserting the notification contract.
struct RecordingObserver final : DocumentObserver {
    std::vector<DocumentChange::Kind> kinds;
    int count = 0;
    void onDocumentChanged(const Document&, const DocumentChange& c) override {
        kinds.push_back(c.kind);
        ++count;
    }
    [[nodiscard]] bool saw(DocumentChange::Kind k) const {
        for (auto x : kinds)
            if (x == k) return true;
        return false;
    }
};

}  // namespace

PE_TEST(history_add_layer_undo_redo) {
    auto doc = Document::createBlank(Size{16, 16});
    auto layer = redFill();
    const LayerId id = layer->id();

    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    PE_CHECK(doc->findLayer(id) != nullptr);
    PE_CHECK(doc->isDirty());
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));

    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK(doc->findLayer(id) == nullptr);
    PE_CHECK(!doc->isDirty());  // back to saved (initial) state

    doc->history().redo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    PE_CHECK(doc->findLayer(id) != nullptr);
    PE_CHECK(doc->isDirty());
}

PE_TEST(history_remove_layer_undo_restores_position) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();

    doc->history().push(std::make_unique<RemoveLayerCommand>(base));
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(0));
    PE_CHECK(doc->findLayer(base) == nullptr);

    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK(doc->findLayer(base) != nullptr);
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
}

PE_TEST(history_reorder_layer) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    auto layer = redFill();
    const LayerId top = layer->id();
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));
    PE_CHECK_EQ(doc->topLevelIndexOf(top), static_cast<std::size_t>(1));

    doc->history().push(std::make_unique<ReorderLayerCommand>(top, 0));
    PE_CHECK_EQ(doc->topLevelIndexOf(top), static_cast<std::size_t>(0));
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(1));

    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelIndexOf(top), static_cast<std::size_t>(1));
    PE_CHECK_EQ(doc->topLevelIndexOf(base), static_cast<std::size_t>(0));
}

PE_TEST(history_duplicate_layer) {
    auto doc = Document::createBlank(Size{16, 16});
    auto layer = redFill();
    const LayerId src = layer->id();
    doc->history().push(std::make_unique<AddLayerCommand>(std::move(layer), doc->topLevelCount()));

    doc->history().push(std::make_unique<DuplicateLayerCommand>(src));
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(3));  // base + src + clone

    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));  // clone removed
    doc->history().redo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(3));  // clone back
}

PE_TEST(history_property_commands_roundtrip) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    Layer* layer = doc->findLayer(base);

    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.25f));
    PE_CHECK_NEAR(layer->opacity(), 0.25f);
    doc->history().undo();
    PE_CHECK_NEAR(layer->opacity(), 1.0f);

    doc->history().push(std::make_unique<SetBlendModeCommand>(base, BlendMode::Screen));
    PE_CHECK(layer->blendMode() == BlendMode::Screen);
    doc->history().undo();
    PE_CHECK(layer->blendMode() == BlendMode::Normal);

    doc->history().push(std::make_unique<SetVisibilityCommand>(base, false));
    PE_CHECK(!layer->visible());
    doc->history().undo();
    PE_CHECK(layer->visible());

    doc->history().push(std::make_unique<RenameLayerCommand>(base, "Renamed"));
    PE_CHECK_EQ(layer->name(), std::string("Renamed"));
    doc->history().undo();
    PE_CHECK_EQ(layer->name(), std::string("Layer 1"));
}

PE_TEST(history_remove_restores_active_layer) {
    // Removing the active layer clears it; undo restores both the layer and the
    // active selection (round-trip of session state).
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    PE_CHECK(base != kNoLayer);

    doc->history().push(std::make_unique<RemoveLayerCommand>(base));
    PE_CHECK_EQ(doc->activeLayer(), kNoLayer);  // cleared on remove

    doc->history().undo();
    PE_CHECK_EQ(doc->activeLayer(), base);  // restored on undo
}

PE_TEST(history_reorder_preserves_active_layer) {
    // Reordering the active layer must NOT deselect it.
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<AddLayerCommand>(redFill(), doc->topLevelCount()));
    doc->setActiveLayer(base);
    PE_CHECK_EQ(doc->activeLayer(), base);

    doc->history().push(std::make_unique<ReorderLayerCommand>(base, 1));
    PE_CHECK_EQ(doc->activeLayer(), base);  // still active after reorder
}

PE_TEST(history_observer_contract) {
    auto doc = Document::createBlank(Size{16, 16});
    RecordingObserver obs;
    doc->addObserver(&obs);

    doc->history().push(std::make_unique<AddLayerCommand>(redFill(), doc->topLevelCount()));
    PE_CHECK(obs.saw(DocumentChange::Kind::LayerStructure));
    PE_CHECK(obs.saw(DocumentChange::Kind::DirtyState));

    // Safe to remove mid-life; further changes are not delivered.
    doc->removeObserver(&obs);
    const int before = obs.count;
    doc->history().push(std::make_unique<SetOpacityCommand>(doc->activeLayer(), 0.5f));
    PE_CHECK_EQ(obs.count, before);
}

PE_TEST(history_saved_marker_dirty_tracking) {
    auto doc = Document::createBlank(Size{16, 16});
    PE_CHECK(!doc->isDirty());

    doc->history().push(std::make_unique<SetOpacityCommand>(doc->activeLayer(), 0.5f));
    PE_CHECK(doc->isDirty());

    doc->history().markSaved();
    PE_CHECK(!doc->isDirty());

    doc->history().push(std::make_unique<SetOpacityCommand>(doc->activeLayer(), 0.25f));
    PE_CHECK(doc->isDirty());

    doc->history().undo();  // back to the saved point
    PE_CHECK(!doc->isDirty());
}

PE_TEST(history_saved_marker_invalidated_by_diverged_branch) {
    // Regression: the saved point must NOT be reported clean once it lies on a redo branch that a
    // fresh edit discarded. save -> undo -> new edit(s) reaching the same depth is a DIFFERENT
    // document and must read dirty (else the app skips its save-on-close prompt = silent data
    // loss).
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.5f));
    doc->history().markSaved();  // saved at depth 1
    PE_CHECK(!doc->isDirty());

    doc->history().undo();  // depth 0 (saved point is now ahead, on the redo branch)
    PE_CHECK(doc->isDirty());
    doc->history().push(
        std::make_unique<SetOpacityCommand>(base, 0.9f));  // discards the redo branch
    // Back at depth 1, but it's a different command than the saved one — must be dirty, not clean.
    PE_CHECK(doc->isDirty());

    // And it stays dirty even if more edits return to/around the old saved depth.
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.8f));
    PE_CHECK(doc->isDirty());
    doc->history().undo();
    PE_CHECK(doc->isDirty());
}

PE_TEST(history_limit_trims_oldest) {
    auto doc = Document::createBlank(Size{16, 16});
    doc->history().setLimit(2);
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.9f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.8f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.7f));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(2));
}

PE_TEST(history_entry_names_for_panel) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.5f));  // "Change Opacity"
    doc->history().push(
        std::make_unique<SetVisibilityCommand>(base, false));  // "Toggle Visibility"

    auto un = doc->history().undoNames();
    PE_CHECK_EQ(un.size(), static_cast<std::size_t>(2));
    PE_CHECK_EQ(un[0], std::string("Change Opacity"));     // oldest first
    PE_CHECK_EQ(un[1], std::string("Toggle Visibility"));  // newest last
    PE_CHECK(doc->history().redoNames().empty());

    doc->history().undo();
    auto rn = doc->history().redoNames();
    PE_CHECK_EQ(rn.size(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(rn[0], std::string("Toggle Visibility"));  // next command redo() replays
    PE_CHECK_EQ(doc->history().undoNames().size(), static_cast<std::size_t>(1));
}

PE_TEST(history_new_edit_truncates_redo) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.5f));
    doc->history().undo();
    PE_CHECK(doc->history().canRedo());
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.3f));
    PE_CHECK(!doc->history().canRedo());  // redo branch discarded
}

PE_TEST(remove_group_clears_dangling_nested_active_layer) {
    // Active layer is a descendant of a group; removing the group must clear the active
    // id (not leave it dangling at a destroyed layer), and undo must restore it.
    auto doc = Document::createBlank(Size{16, 16});
    auto group = std::make_unique<GroupLayer>("Grp");
    auto child = std::make_unique<PixelLayer>("Inner");
    const LayerId childId = child->id();
    group->addChild(std::move(child));
    const LayerId groupId = group->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
    doc->setActiveLayer(childId);
    PE_CHECK_EQ(doc->activeLayer(), childId);

    doc->history().push(std::make_unique<RemoveLayerCommand>(groupId));
    PE_CHECK(doc->findLayer(childId) == nullptr);  // the nested layer is gone
    PE_CHECK_EQ(doc->activeLayer(), kNoLayer);     // active was cleared, not dangling

    doc->history().undo();
    PE_CHECK(doc->findLayer(childId) != nullptr);  // subtree restored
    PE_CHECK_EQ(doc->activeLayer(), childId);      // active restored
}

namespace {

// A command that mutates, then optionally throws before returning. Models the real
// pattern the issue names: GroupLayersCommand allocates a vector after it has begun
// removing layers, AddLayerMaskCommand allocates a Mask and does a canvas-wide fillRect,
// and CropCommand allocates a move command per layer. All are bad_alloc paths, and this
// codebase runs near memory limits by design (a 3.7 GiB default content budget).
class PartialThenThrow final : public Command {
public:
    PartialThenThrow(PixelLayer* layer, bool throwOnExecute, bool throwOnUndo)
        : layer_(layer), throwOnExecute_(throwOnExecute), throwOnUndo_(throwOnUndo) {}

    [[nodiscard]] std::string name() const override { return "Partial"; }

    DocumentChange execute(Document&) override {
        layer_->tiles().setPixel(0, 0, kRed);  // the partial mutation
        mutated_ = true;
        if (throwOnExecute_) throw std::bad_alloc{};
        return DocumentChange{DocumentChange::Kind::Pixels, Rect{0, 0, 1, 1}, layer_->id()};
    }

    DocumentChange undo(Document&) override {
        if (throwOnUndo_) throw std::bad_alloc{};
        // Tolerant of a partially-completed execute, which is the contract History relies
        // on when it keeps a throwing command on the undo stack.
        if (mutated_) {
            layer_->tiles().setPixel(0, 0, Rgba8{});
            mutated_ = false;
        }
        return DocumentChange{DocumentChange::Kind::Pixels, Rect{0, 0, 1, 1}, layer_->id()};
    }

private:
    PixelLayer* layer_;
    bool throwOnExecute_;
    bool throwOnUndo_;
    bool mutated_ = false;
};

}  // namespace

PE_TEST(history_keeps_a_command_whose_execute_throws) {
    // push() executed the command and then moved it onto the stack, with nothing between.
    // A throw partway through execute destroyed the command with its partial mutation
    // still applied and on neither stack, so the document was permanently un-undoable
    // past that point: the one object that knew how to revert the change was gone.
    auto doc = Document::createBlank(Size{16, 16});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));

    bool threw = false;
    try {
        doc->history().push(std::make_unique<PartialThenThrow>(pl, true, false));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    PE_CHECK(threw);
    PE_CHECK_EQ(pl->tiles().pixel(0, 0), kRed);  // the partial mutation is applied

    // The command survives on the undo stack, so the mutation is still reversible.
    PE_CHECK(doc->history().canUndo());
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));
    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(0, 0), (Rgba8{}));  // reverted
}

PE_TEST(history_keeps_a_command_whose_undo_throws) {
    // undo() pops before calling undo(), so a throw there dropped the command from both
    // stacks the same way.
    auto doc = Document::createBlank(Size{16, 16});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    doc->history().push(std::make_unique<PartialThenThrow>(pl, false, true));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));

    bool threw = false;
    try {
        doc->history().undo();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    PE_CHECK(threw);
    // The undo did not complete, so the command belongs where it was, not on the redo
    // stack: a subsequent undo must retry it rather than skip past it.
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));
    PE_CHECK(!doc->history().canRedo());
}

PE_TEST(history_notifies_observers_after_a_throwing_execute) {
    // The document really did change, so anything showing it must refresh. The extent is
    // unknown at that point, so the notification has to be the conservative one.
    auto doc = Document::createBlank(Size{16, 16});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    RecordingObserver obs;
    doc->addObserver(&obs);
    try {
        doc->history().push(std::make_unique<PartialThenThrow>(pl, true, false));
    } catch (const std::bad_alloc&) {
    }
    PE_CHECK(obs.count > 0);
    PE_CHECK(obs.saw(DocumentChange::Kind::LayerStructure));
    doc->removeObserver(&obs);
}
