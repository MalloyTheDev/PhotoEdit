#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <memory>

using namespace pe;

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
