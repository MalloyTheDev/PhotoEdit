#pragma once

#include "pe/core/Document.hpp"

#include <QWidget>

class QLabel;

namespace pe::app {

// The Properties dock: a read-only summary of the active document's canvas, like
// Photoshop's Properties panel with no selection. It presents the immutable facts
// of the document — pixel dimensions, resolution, color mode, and storage depth —
// grouped under small section headers with labeled value rows.
//
// It observes the document so any committed change (or a file load / document swap)
// refreshes the displayed values. Nothing here mutates the document; editing the
// canvas size or resolution belongs to a future Image > Canvas Size command.
class PropertiesPanel : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);
    ~PropertiesPanel() override;

    // Observe and show `doc`, or detach and clear when null. Must be called with
    // null before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

private:
    void refresh();  // pull current values into the labels (dashes when no doc)

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null

    QLabel* width_ = nullptr;
    QLabel* height_ = nullptr;
    QLabel* resolution_ = nullptr;
    QLabel* mode_ = nullptr;
    QLabel* depth_ = nullptr;
    QLabel* units_ = nullptr;
};

}  // namespace pe::app
