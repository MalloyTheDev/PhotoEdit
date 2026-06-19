#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <memory>

using namespace pe;

namespace {
// Counts the DocumentChange kinds an observer receives, to assert notification hygiene.
struct CountingObserver : DocumentObserver {
    int selection = 0;
    int structure = 0;
    int total = 0;
    void onDocumentChanged(const Document&, const DocumentChange& c) override {
        ++total;
        if (c.kind == DocumentChange::Kind::Selection) ++selection;
        if (c.kind == DocumentChange::Kind::LayerStructure) ++structure;
    }
};
}  // namespace

PE_TEST(setselection_apply_undo_redo_roundtrip) {
    auto doc = Document::createBlank(Size{64, 64});
    PE_CHECK(!doc->selection().active());  // inactive (whole canvas editable) by default

    Selection sel;
    sel.selectRect(Rect{8, 8, 16, 16});
    doc->history().push(std::make_unique<SetSelectionCommand>(sel));

    PE_CHECK(doc->selection().active());
    PE_CHECK_EQ(static_cast<int>(doc->selection().value(12, 12)), 255);  // inside
    PE_CHECK_EQ(static_cast<int>(doc->selection().value(40, 40)), 0);    // outside

    doc->history().undo();
    PE_CHECK(!doc->selection().active());  // restored exactly to the prior (none)

    doc->history().redo();
    PE_CHECK(doc->selection().active());
    PE_CHECK_EQ(static_cast<int>(doc->selection().value(12, 12)), 255);
}

PE_TEST(setselection_large_canvas_no_truncation) {
    // Regression: the command once snapshotted a fixed 4096x4096 mask, silently
    // dropping any selection beyond 4096px on apply AND undo. Selecting far past
    // 4096 must survive apply -> undo -> redo exactly.
    auto doc = Document::createBlank(Size{8000, 8000});
    Selection sel;
    sel.selectRect(Rect{5000, 5000, 100, 100});
    doc->history().push(std::make_unique<SetSelectionCommand>(sel));
    PE_CHECK_EQ(static_cast<int>(doc->selection().value(5050, 5050)), 255);  // 0 under the old cap

    doc->history().undo();
    PE_CHECK(!doc->selection().active());

    doc->history().redo();
    PE_CHECK_EQ(static_cast<int>(doc->selection().value(5050, 5050)), 255);
}

PE_TEST(setselection_notifies_selection_exactly_once) {
    // Each SetSelectionCommand push must broadcast exactly ONE Selection change. It used to fire
    // two (a self-notify via touchSelection AND the change History broadcasts), making every
    // selection edit recompute the marching-ants bounds twice.
    auto doc = Document::createBlank(Size{64, 64});
    CountingObserver obs;
    doc->addObserver(&obs);

    Selection sel;
    sel.selectRect(Rect{8, 8, 16, 16});
    doc->history().push(std::make_unique<SetSelectionCommand>(sel));
    PE_CHECK_EQ(obs.selection, 1);  // not 2

    doc->history().undo();
    PE_CHECK_EQ(obs.selection, 2);  // one more for the undo (still exactly one per push)

    doc->removeObserver(&obs);
}

PE_TEST(crop_still_notifies_both_selection_and_structure) {
    // Guards the distinction the SetSelectionCommand fix relies on: CropCommand returns a
    // LayerStructure change AND self-notifies Selection (it shifts the selection with the canvas),
    // so it must still broadcast BOTH kinds — its touchSelection() is load-bearing, not redundant.
    auto doc = Document::createBlank(Size{64, 64});
    Selection sel;
    sel.selectRect(Rect{10, 10, 40, 40});
    doc->editableSelection() = sel;  // set directly (no notify) so the counter starts clean

    CountingObserver obs;
    doc->addObserver(&obs);
    doc->history().push(
        std::make_unique<CropCommand>(Rect{8, 8, 40, 40}));  // origin offset -> shift
    PE_CHECK(obs.selection >= 1);  // selection tracked the crop (Selection notify)
    PE_CHECK(obs.structure >= 1);  // canvas resized (LayerStructure notify)
    doc->removeObserver(&obs);
}
