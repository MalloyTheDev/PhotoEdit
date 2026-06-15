#pragma once

#include "pe/core/Document.hpp"
#include "pe/core/PaintToolController.hpp"

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QWidget>

namespace pe::app {

// The central canvas: shows a document's flattened composite and routes mouse input
// to the brush tool. It observes the document, so committed edits (paint commits,
// undo/redo, file loads) refresh automatically; the live brush preview repaints
// explicitly as it is drawn. The tiled/zoomable viewport
// (docs/systems/02-canvas-rendering.md) replaces this centered-100% display later.
class CanvasView : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit CanvasView(QWidget* parent = nullptr);
    ~CanvasView() override;

    // Observe and show `doc`, or detach and clear when null. Safe to call repeatedly;
    // must be called with null before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    // Brush/eraser settings, for future tool-options UI (size, color, mode).
    [[nodiscard]] pe::PaintToolController& tool() noexcept { return tool_; }

    // DocumentObserver: re-flatten and repaint after any committed change.
    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    void refreshImage();                                // re-flatten doc_ -> image_
    [[nodiscard]] QPoint imageOrigin() const noexcept;  // top-left of the centered image
    [[nodiscard]] pe::StrokePoint sampleAt(QPointF widgetPos) const;  // widget -> doc space

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    QImage image_;                 // a private copy of the current composite (RGBA8888)
    pe::PaintToolController tool_;
};

}  // namespace pe::app
