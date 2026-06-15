#include "HistoryPanel.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/History.hpp"

#include <QColor>
#include <QListWidget>
#include <QVBoxLayout>

namespace pe::app {

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget(this);
    root->addWidget(list_);
    connect(list_, &QListWidget::currentRowChanged, this, &HistoryPanel::onRowActivated);
    rebuild();
}

HistoryPanel::~HistoryPanel() {
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void HistoryPanel::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    rebuild();
}

void HistoryPanel::onDocumentChanged(const pe::Document&, const pe::DocumentChange&) {
    if (seeking_) return;  // our own seek loop drives a single rebuild at the end
    rebuild();
}

void HistoryPanel::rebuild() {
    const bool prev = seeking_;
    seeking_ = true;  // programmatic repopulation must not echo as a user seek
    list_->clear();
    if (doc_ != nullptr) {
        const pe::History& h = doc_->history();
        const std::vector<std::string> undo = h.undoNames();  // oldest -> newest
        const std::vector<std::string> redo = h.redoNames();  // next-to-redo first

        list_->addItem(QStringLiteral("Open"));  // row 0 == initial state (depth 0)
        for (const std::string& n : undo) list_->addItem(QString::fromStdString(n));
        for (const std::string& n : redo) list_->addItem(QString::fromStdString(n));

        const int current = static_cast<int>(undo.size());  // current depth
        list_->setCurrentRow(current);
        // Dim the future (redoable) states below the current one.
        for (int i = current + 1; i < list_->count(); ++i) {
            if (QListWidgetItem* it = list_->item(i)) it->setForeground(QColor(Qt::gray));
        }
    }
    seeking_ = prev;
}

void HistoryPanel::onRowActivated(int row) {
    if (seeking_ || doc_ == nullptr || row < 0) return;
    pe::History& h = doc_->history();
    const int current = static_cast<int>(h.undoDepth());
    if (row == current) return;

    seeking_ = true;  // suppress per-step observer rebuilds during the walk
    if (row < current) {
        for (int i = 0; i < current - row; ++i) h.undo();
    } else {
        for (int i = 0; i < row - current; ++i) h.redo();
    }
    seeking_ = false;
    rebuild();
}

}  // namespace pe::app
