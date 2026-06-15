#pragma once

#include "pe/core/Document.hpp"

#include <QWidget>

#include <memory>

class QComboBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;

namespace pe {
class Command;
}

namespace pe::app {

// The Layers dock: presents the document's top-level layer stack (top layer first,
// as in every layered editor) and edits it through undoable commands — visibility,
// opacity, blend mode, add, duplicate, delete, and reorder. Nested groups, masks,
// thumbnails, and drag-reorder arrive later.
//
// It observes the document, so external edits (undo/redo, file loads, structural
// changes from anywhere) keep the panel in sync. All mutations go through
// History, so the canvas (also an observer) recomposites automatically.
class LayersPanel : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit LayersPanel(QWidget* parent = nullptr);
    ~LayersPanel() override;

    // Observe and show `doc`, or detach and clear when null. Must be called with
    // null before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

private:
    void rebuild();             // repopulate the list from the model
    void syncActiveControls();  // reflect the active layer into blend/opacity
    void updateButtons();       // enable/disable per current selection
    void selectActiveInList();  // highlight the row matching the active layer
    [[nodiscard]] pe::LayerId selectedLayer() const;

    void onRowChanged();
    void onItemChanged(QListWidgetItem* item);  // visibility checkbox
    void onBlendChanged(int index);
    void onOpacityEdited();
    void onAdd();
    void onDuplicate();
    void onDelete();
    void onMoveUp();
    void onMoveDown();

    void push(std::unique_ptr<pe::Command> cmd);  // doc_->history().push, if any doc

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    bool updating_ = false;        // guard: programmatic UI updates must not echo as edits

    QComboBox* blend_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* addBtn_ = nullptr;
    QPushButton* dupBtn_ = nullptr;
    QPushButton* delBtn_ = nullptr;
    QPushButton* upBtn_ = nullptr;
    QPushButton* downBtn_ = nullptr;
};

}  // namespace pe::app
