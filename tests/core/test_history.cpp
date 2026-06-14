#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <memory>
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
    PE_CHECK_EQ(layer->name(), std::string("Background"));
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

PE_TEST(history_limit_trims_oldest) {
    auto doc = Document::createBlank(Size{16, 16});
    doc->history().setLimit(2);
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.9f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.8f));
    doc->history().push(std::make_unique<SetOpacityCommand>(base, 0.7f));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(2));
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
