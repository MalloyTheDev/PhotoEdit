#pragma once

#include "pe/core/Document.hpp"

#include <QWidget>

class QListWidget;

namespace pe::app {

// The History dock: a timeline of document states. Row 0 is the opened/initial
// state; each following row is one applied command (oldest first), with the
// already-undone (redoable) states shown dimmed below the current one. The current
// state is highlighted. Selecting a row seeks there by undoing/redoing through
// History, so it is fully reversible.
//
// Listing depends on History::undoNames()/redoNames(); jumping uses the one-step
// undo()/redo() in a loop. Observes the document to stay in sync with edits made
// anywhere (paint, layers, menu undo/redo).
class HistoryPanel : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit HistoryPanel(QWidget* parent = nullptr);
    ~HistoryPanel() override;

    // Observe and show `doc`, or detach and clear when null. Must be called with
    // null before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

private:
    void rebuild();                // repopulate the timeline from History
    void onRowActivated(int row);  // seek to the selected state

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    bool seeking_ = false;         // guard: our own seek / programmatic selection
    QListWidget* list_ = nullptr;
};

}  // namespace pe::app
