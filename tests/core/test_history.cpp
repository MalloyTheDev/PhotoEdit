#include <cstdint>
#include "pe/core/Brush.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/History.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
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

namespace {

// A command that claims a fixed size, so a budget test can be about the TRIMMING rather
// than about how many tiles a stroke happens to touch.
class Weighty final : public Command {
public:
    Weighty(int* liveCount, std::int64_t bytes) : live_(liveCount), bytes_(bytes) { ++*live_; }
    ~Weighty() override { --*live_; }
    [[nodiscard]] std::string name() const override { return "Weighty"; }
    DocumentChange execute(Document&) override { return DocumentChange{}; }
    DocumentChange undo(Document&) override { return DocumentChange{}; }
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override { return bytes_; }

private:
    int* live_;
    std::int64_t bytes_;
};

}  // namespace

PE_TEST(history_trims_on_bytes_not_only_on_step_count) {
    // The step count never bound anything useful: one stroke can retain tens of megabytes
    // and a single one can reach about a gigabyte, so a hundred-step limit does not engage
    // until far past what the machine has.
    auto doc = Document::createBlank(Size{16, 16});
    History& h = doc->history();
    h.setLimit(1000);            // effectively out of the way
    h.setByteBudget(10 * 1024);  // 10 KiB

    int live = 0;
    for (int i = 0; i < 8; ++i) {
        h.push(std::make_unique<Weighty>(&live, 4 * 1024));  // 4 KiB each
    }
    // Three at 4 KiB would be 12 KiB, over budget, so only two are kept.
    PE_CHECK_EQ(h.undoDepth(), static_cast<std::size_t>(2));
    PE_CHECK(h.retainedBytes() <= 10 * 1024);
    // And the trimmed commands are really gone, not merely unreachable.
    PE_CHECK_EQ(live, 2);
}

PE_TEST(history_keeps_one_step_even_if_it_alone_exceeds_the_budget) {
    // A stroke the user just made and cannot undo is worse than briefly exceeding a soft
    // limit, and one command can legitimately be larger than the whole budget.
    auto doc = Document::createBlank(Size{16, 16});
    History& h = doc->history();
    h.setByteBudget(1024);

    int live = 0;
    h.push(std::make_unique<Weighty>(&live, 64 * 1024));  // 64x the budget
    PE_CHECK_EQ(h.undoDepth(), static_cast<std::size_t>(1));
    PE_CHECK(h.canUndo());
    PE_CHECK(h.retainedBytes() > h.byteBudget());  // deliberately over, and reported as such
}

PE_TEST(history_byte_budget_accounts_for_the_redo_branch) {
    // Undone commands are still resident, so they have to count. Otherwise undoing a long
    // painting session would look like it freed memory while holding all of it.
    auto doc = Document::createBlank(Size{16, 16});
    History& h = doc->history();
    h.setLimit(1000);
    h.setByteBudget(0);  // unlimited while we set the situation up

    int live = 0;
    for (int i = 0; i < 4; ++i) h.push(std::make_unique<Weighty>(&live, 4 * 1024));
    const std::int64_t all = h.retainedBytes();
    PE_CHECK_EQ(all, 16 * 1024);

    h.undo();
    h.undo();
    PE_CHECK_EQ(h.undoDepth(), static_cast<std::size_t>(2));
    PE_CHECK_EQ(h.redoDepth(), static_cast<std::size_t>(2));
    PE_CHECK_EQ(h.retainedBytes(), all);  // moving between stacks frees nothing

    // Applying a budget now trims the undo stack first and then the redo branch.
    h.setByteBudget(4 * 1024);
    PE_CHECK(h.retainedBytes() <= 4 * 1024);
    PE_CHECK_EQ(h.undoDepth(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(h.redoDepth(), static_cast<std::size_t>(0));
}

PE_TEST(history_push_stops_counting_the_discarded_redo_branch) {
    // A new edit throws the redo branch away; if its bytes were not subtracted the total
    // would drift upward forever and trim a healthy history for no reason.
    auto doc = Document::createBlank(Size{16, 16});
    History& h = doc->history();
    h.setByteBudget(0);
    int live = 0;
    for (int i = 0; i < 3; ++i) h.push(std::make_unique<Weighty>(&live, 4 * 1024));
    h.undo();
    h.undo();
    PE_CHECK_EQ(h.redoDepth(), static_cast<std::size_t>(2));

    h.push(std::make_unique<Weighty>(&live, 4 * 1024));  // discards the redo branch
    PE_CHECK_EQ(h.redoDepth(), static_cast<std::size_t>(0));
    PE_CHECK_EQ(h.undoDepth(), static_cast<std::size_t>(2));
    PE_CHECK_EQ(h.retainedBytes(), 8 * 1024);  // exactly the two that remain
    PE_CHECK_EQ(live, 2);
}

PE_TEST(history_byte_budget_of_zero_is_unlimited) {
    auto doc = Document::createBlank(Size{16, 16});
    History& h = doc->history();
    h.setLimit(0);
    h.setByteBudget(0);
    int live = 0;
    for (int i = 0; i < 50; ++i) h.push(std::make_unique<Weighty>(&live, 1024 * 1024));
    PE_CHECK_EQ(h.undoDepth(), static_cast<std::size_t>(50));
    PE_CHECK_EQ(h.retainedBytes(), 50LL * 1024 * 1024);
}

PE_TEST(history_default_budget_is_documented_and_applied) {
    auto doc = Document::createBlank(Size{16, 16});
    PE_CHECK_EQ(doc->history().byteBudget(), kDefaultHistoryBytes);
    PE_CHECK(kDefaultHistoryBytes > 0);
}

PE_TEST(paint_and_selection_commands_report_what_they_retain) {
    // Command::retainedBytes defaults to zero, which is right for a command holding a
    // handful of scalars and wrong for one holding pixels. The ones that hold real memory
    // have to say so, or the budget silently bounds nothing.
    auto doc = Document::createBlank(Size{512, 512});
    const LayerId id = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(id));
    pl->tiles().fillRect(Rect{0, 0, 512, 512}, Rgba8{10, 20, 30, 255});

    BrushSettings b;
    b.diameter = 40.0f;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    const std::vector<StrokePoint> pts{StrokePoint{Vec2{40.0f, 40.0f}, 1.0f},
                                       StrokePoint{Vec2{400.0f, 400.0f}, 1.0f}};
    auto paint = paintStroke(*doc, id, b, Rgbaf{1, 0, 0, 1}, pts, nullptr);
    PE_CHECK(paint != nullptr);
    // A stroke crossing several tiles retains at least one tile's worth per tile touched.
    const std::int64_t perTile =
        static_cast<std::int64_t>(kTilePixels) * static_cast<std::int64_t>(sizeof(Rgba8));
    PE_CHECK(paint->retainedBytes() >= perTile);
    PE_CHECK(paint->retainedBytes() % perTile == 0);

    Selection sel;
    sel.selectRect(Rect{0, 0, 512, 512});
    auto setSel = std::make_unique<SetSelectionCommand>(std::move(sel));
    setSel->execute(*doc);  // captures the previous selection too
    PE_CHECK(setSel->retainedBytes() >= static_cast<std::int64_t>(kTilePixels));
}

PE_TEST(history_byte_budget_trims_real_paint_commands) {
    // The end-to-end case the issue is actually about: painting, not synthetic weights.
    auto doc = Document::createBlank(Size{1024, 1024});
    const LayerId id = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(id));
    pl->tiles().fillRect(Rect{0, 0, 1024, 1024}, Rgba8{10, 20, 30, 255});
    History& h = doc->history();
    h.setLimit(1000);
    h.setByteBudget(4 * 1024 * 1024);  // 4 MiB, about 16 tiles at 8-bit

    BrushSettings b;
    b.diameter = 30.0f;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    for (int i = 0; i < 12; ++i) {
        const auto y = static_cast<float>(60 + i * 70);
        const std::vector<StrokePoint> pts{StrokePoint{Vec2{40.0f, y}, 1.0f},
                                           StrokePoint{Vec2{980.0f, y}, 1.0f}};
        auto cmd = paintStroke(*doc, id, b, Rgbaf{1, 0, 0, 1}, pts, nullptr);
        PE_CHECK(cmd != nullptr);
        h.push(std::move(cmd));
    }
    PE_CHECK(h.undoDepth() < static_cast<std::size_t>(12));  // it really trimmed
    PE_CHECK(h.undoDepth() >= static_cast<std::size_t>(1));
    PE_CHECK(h.retainedBytes() <= h.byteBudget());
    PE_CHECK(h.canUndo());  // and the most recent strokes are still undoable
}

PE_TEST(history_save_point_survives_edits_made_while_the_save_runs) {
    // The ordinary case the two-phase save exists for: a snapshot is taken, the user paints
    // while the worker writes, and the file holds the earlier state. Committing must mark
    // THAT state saved and leave the later strokes dirty.
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.5f));

    const std::uint64_t token = doc->history().beginSave();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.4f));  // painted mid-save
    doc->history().commitSave(token);

    PE_CHECK(doc->isDirty());  // the stroke made during the save is not on disk
    doc->history().undo();     // back to exactly what was written
    PE_CHECK(!doc->isDirty());
}

PE_TEST(history_save_point_is_dropped_when_its_branch_is_discarded) {
    // A save runs off a snapshot with the canvas live, and undo/redo stay enabled for the
    // duration, so the stack can BRANCH while the worker writes. The saved depth used to be
    // a plain integer the caller held across that: undo twice, paint back up to the same
    // depth, and committing the number marked a document clean that shared nothing with the
    // file but its depth. confirmDiscard then discarded the strokes with no prompt.
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.9f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.8f));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(2));

    const std::uint64_t token = doc->history().beginSave();  // branch X, depth 2
    doc->history().undo();
    doc->history().undo();
    // Two fresh edits: the redo branch is discarded and the depth returns to 2 on branch Y.
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.7f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.6f));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(2));

    doc->history().commitSave(token);
    PE_CHECK(doc->isDirty());  // branch Y was never written, whatever its depth
}

PE_TEST(history_save_point_follows_the_stack_when_trimming_reindexes_it) {
    // The other way an absolute depth goes stale, and it needs no undo at all: one stroke
    // pushed onto a stack already at limit() drops the oldest command, so every index shifts
    // down by one. Committing the pre-shift number marked the stroke made DURING the save as
    // saved. limit() is 100 by default and nothing raises it, so this is reachable from a
    // long editing session.
    auto doc = Document::createBlank(Size{16, 16});
    doc->history().setLimit(2);
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.9f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.8f));  // at the limit

    const std::uint64_t token = doc->history().beginSave();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.7f));  // trims the oldest
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(2));  // same depth, shifted

    doc->history().commitSave(token);
    PE_CHECK(doc->isDirty());  // the stroke made during the save is still unsaved
    doc->history().undo();     // back to what the file holds
    PE_CHECK(!doc->isDirty());
}

PE_TEST(history_save_point_is_dropped_when_it_falls_off_the_front) {
    // Trimmed far enough that the written state is no longer on the stack at all. It can
    // never be returned to, so it must not be marked: same sentinel the committed marker
    // uses, and the document stays dirty.
    auto doc = Document::createBlank(Size{16, 16});
    doc->history().setLimit(2);
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.9f));

    const std::uint64_t token = doc->history().beginSave();  // depth 1
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.8f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.7f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.6f));

    doc->history().commitSave(token);
    PE_CHECK(doc->isDirty());
    doc->history().undo();
    PE_CHECK(doc->isDirty());  // and no depth on this stack reads clean
    doc->history().undo();
    PE_CHECK(doc->isDirty());
}

PE_TEST(history_abandoned_and_stale_save_tokens_mark_nothing) {
    auto doc = Document::createBlank(Size{16, 16});
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.5f));

    // A save that failed or threw abandons its point; committing it afterwards must not
    // mark the document clean on the strength of a write that never happened.
    const std::uint64_t failed = doc->history().beginSave();
    doc->history().abandonSave(failed);
    doc->history().commitSave(failed);
    PE_CHECK(doc->isDirty());

    // A token superseded by a later beginSave() no longer owns the marker.
    const std::uint64_t first = doc->history().beginSave();
    const std::uint64_t second = doc->history().beginSave();
    PE_CHECK(first != second);
    doc->history().commitSave(first);
    PE_CHECK(doc->isDirty());
    doc->history().commitSave(second);
    PE_CHECK(!doc->isDirty());  // the current one does mark it
}
