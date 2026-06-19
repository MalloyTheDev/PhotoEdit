#pragma once

#include "pe/core/Document.hpp"

#include <QWidget>

#include <cstddef>
#include <memory>
#include <set>
#include <span>
#include <vector>

class QComboBox;
class QIcon;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QSpinBox;

namespace pe {
class Command;
class Layer;
}  // namespace pe

namespace pe::app {

// The Layers dock: presents the document's layer tree (top layer first, as in every
// layered editor) with groups shown as expandable parents, and edits it through
// undoable commands — visibility, opacity, blend mode, add, duplicate, delete,
// reorder, and group / ungroup. Per-layer property edits (visibility/opacity/blend)
// work at any depth; add / duplicate / delete / reorder operate on the top level in
// this version (nested reordering and drag-drop arrive with the engine commands for it).
//
// It observes the document, so external edits (undo/redo, file loads, structural
// changes from anywhere) keep the panel in sync. All mutations go through History,
// so the canvas (also an observer) recomposites automatically.
class LayersPanel : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit LayersPanel(QWidget* parent = nullptr);
    ~LayersPanel() override;

    // Observe and show `doc`, or detach and clear when null. Must be called with
    // null before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

    // Group the multi-selected top-level layers into a new group; dissolve the active
    // top-level group. Both are safe no-ops when the selection doesn't qualify.
    // MainWindow wires the Layer▸Group (Ctrl+G) / Ungroup (Ctrl+Shift+G) actions here.
    void groupSelected();
    void ungroupSelected();

private:
    void rebuild();             // repopulate the tree from the model
    void syncActiveControls();  // reflect the active layer into blend/opacity
    void updateButtons();       // enable/disable per current selection
    void selectActiveInTree();  // make the active layer the current item
    // Build the items for one level (recursing into groups). `siblings` is the engine
    // span owning the layers at this level; items are added top-first.
    void addLevel(QTreeWidgetItem* parentItem,
                  std::span<const std::unique_ptr<pe::Layer>> siblings);
    [[nodiscard]] QTreeWidgetItem* itemForId(pe::LayerId id) const;  // depth-first lookup

    [[nodiscard]] pe::LayerId selectedLayer() const;  // the current (active) item's id
    // Ids of the multi-selection that are top-level layers (the only ones groupable here).
    [[nodiscard]] std::vector<pe::LayerId> selectedTopLevelIds() const;
    // True iff there is a selection and every selected item is a top-level sibling.
    [[nodiscard]] bool selectionAllTopLevel() const;

    // A small preview of one layer (composited alone), for the row icon; groups get a
    // folder glyph. `siblings`/`index` give the engine slot to composite.
    [[nodiscard]] QIcon layerThumbnail(std::span<const std::unique_ptr<pe::Layer>> siblings,
                                       std::size_t index) const;
    [[nodiscard]] QIcon groupIcon() const;
    [[nodiscard]] QIcon adjustmentIcon() const;  // glyph for non-pixel adjustment layers
    // Refresh just one layer's row icon after a pixel edit (the paint hot path),
    // instead of rebuilding the whole tree. Falls back to rebuild() if not found.
    void updateLayerThumbnail(pe::LayerId id);

    void onRowChanged();
    void onSelectionChanged();
    void onItemChanged(QTreeWidgetItem* item, int column);  // visibility checkbox
    void onItemExpanded(QTreeWidgetItem* item);
    void onItemCollapsed(QTreeWidgetItem* item);
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
    // Set while onRowChanged() pushes the active layer to the document. The resulting
    // (synchronous) ActiveLayer notification must NOT re-select the active item — doing so
    // would clear the rest of the multi-selection (setCurrentItem uses ClearAndSelect),
    // making it impossible to assemble a 2+ layer selection for Group. The UI is already
    // in sync in that case; only externally-driven active changes need re-selection.
    bool syncingActive_ = false;

    QComboBox* blend_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* addBtn_ = nullptr;
    QPushButton* dupBtn_ = nullptr;
    QPushButton* delBtn_ = nullptr;
    QPushButton* groupBtn_ = nullptr;
    QPushButton* ungroupBtn_ = nullptr;
    QPushButton* upBtn_ = nullptr;
    QPushButton* downBtn_ = nullptr;

    // Groups the user has collapsed (by id). Everything else defaults to expanded so a
    // rebuild after an edit does not fold the tree back up. Survives rebuilds.
    std::set<pe::LayerId> collapsed_;
};

}  // namespace pe::app
