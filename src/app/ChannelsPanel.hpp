#pragma once

#include "pe/core/Channels.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <QWidget>

#include <functional>

class QEvent;
class QObject;
class QTreeWidget;
class QTreeWidgetItem;

namespace pe::app {

// The Channels dock: the document's colour planes, and which of them the canvas shows.
//
// This dock held a centred label and nothing else. What it shows now is the RGB composite
// and the Red, Green and Blue planes, each with a thumbnail of that plane and an eye. The
// eyes combine, so hiding one channel leaves the other two in colour, and viewing exactly
// one shows it as grey, which is how a channel is actually read: looking for which plane
// carries the noise, or which one makes the cleanest mask. The rule itself lives in the
// engine (pe::applyChannelView) so it is testable headlessly and so the canvas and these
// thumbnails cannot disagree about what a channel looks like.
//
// Not here yet, because the engine has no model for them: spot channels, and saved
// selections stored as alpha channels. docs/systems/19-channels.md specifies both.
//
// Visibility is display state. Nothing this panel does edits the document or touches the
// undo stack, which is why it emits a view rather than pushing a command.
class ChannelsPanel : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit ChannelsPanel(QWidget* parent = nullptr);
    ~ChannelsPanel() override;

    // Observe and show `doc`, or detach and clear when null. Must be called with null
    // before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    // Where the row thumbnails come from: a bounded, downscaled composite of the whole
    // canvas, at most `maxPixels` of output. A callable rather than a document read because
    // the only affordable source is the canvas renderer's tile cache, and the panel has no
    // business owning a renderer. Document::compositeImage() is the wrong answer: it
    // flattens the entire canvas and returns nothing at all past its megapixel cap, which
    // is exactly how the layer thumbnails came to vanish on large documents (#146).
    void setPreviewSource(std::function<pe::PixelBuffer(int maxPixels)> source);

    [[nodiscard]] pe::ChannelView view() const noexcept { return view_; }
    // Set the view from outside. Updates the eyes but deliberately does not emit, so pushing
    // the canvas's state in cannot echo back out as a fresh request.
    void setView(pe::ChannelView v);

    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

signals:
    void viewChanged(pe::ChannelView view);

protected:
    // Thumbnails are only rebuilt while the dock is on screen; this catches up when it is
    // raised. The Channels dock shares a tab group with Layers, so it is usually hidden, and
    // recompositing a bounded preview on every stroke of a document nobody is looking at is
    // pure waste (#156 is the same lesson).
    void showEvent(QShowEvent* e) override;
    // Return on a row views that channel alone. QTreeWidget::itemActivated also fires for a
    // single click under some styles, which would fight the click handler.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Row order is fixed: composite first, then the planes in R, G, B order.
    enum Row { Composite = 0, Red, Green, Blue, kRowCount };

    void buildRows();
    void syncEyes();           // push view_ into the checkboxes without echoing
    void refreshThumbnails();  // pull one preview and split it into the four rows
    void emitView();           // announce view_, guarded against re-entry
    void soloRow(int row);     // view exactly that channel (or everything, for Composite)
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemClicked(QTreeWidgetItem* item, int column);

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    std::function<pe::PixelBuffer(int)> previewSource_;
    pe::ChannelView view_{};
    bool updating_ = false;    // guard: our own writes into the tree
    bool thumbsStale_ = true;  // a change arrived while hidden; catch up on show
    QTreeWidget* tree_ = nullptr;
};

}  // namespace pe::app
