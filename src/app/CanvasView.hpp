#pragma once

#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PaintToolController.hpp"
#include "pe/core/RHI.hpp"
#include "pe/core/ViewTransform.hpp"

#include <QBrush>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QWidget>

#include <memory>

namespace pe::app {

// The central canvas: shows a document's flattened composite through a zoom/pan
// view transform and routes mouse input to the brush tool. It observes the
// document, so committed edits (paint commits, undo/redo, file loads) refresh
// automatically; the live brush preview repaints explicitly as it is drawn.
//
// View navigation: mouse wheel zooms about the cursor, middle-drag pans, and the
// document fits the viewport when first shown. The presentation transform is the
// engine's headless pe::ViewTransform (docs/systems/02-canvas-rendering.md); the
// document is never resampled. GPU display and dirty-region repaint arrive later.
class CanvasView : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    // How the canvas interprets a left-button gesture. Brush/Eraser paint, Hand
    // pans, Zoom clicks to zoom; Inactive is a selected-but-unimplemented tool
    // (clicks do nothing) — the scaffold the rest of the toolset wires into.
    enum class Tool { Brush, Eraser, Hand, Zoom, Marquee, Eyedropper, Move, Lasso, Inactive };

    explicit CanvasView(QWidget* parent = nullptr);
    ~CanvasView() override;

    // Observe and show `doc`, or detach and clear when null. Safe to call repeatedly;
    // must be called with null before the observed document is destroyed.
    void setDocument(pe::Document* doc);

    // Brush/eraser settings, for future tool-options UI (size, color, mode).
    [[nodiscard]] pe::PaintToolController& tool() noexcept { return tool_; }

    // Select the active tool (driven by the tool toolbar). Brush/Eraser also set
    // the paint controller's mode.
    void setTool(Tool t);
    [[nodiscard]] Tool activeTool() const noexcept { return toolMode_; }

signals:
    void zoomChanged(double percent);  // for the status-bar zoom readout
    void colorPicked(const QColor& c); // for eyedropper tool

public:
    // View navigation (also driven by the View menu).
    void fitToWindow();   // scale so the whole document is visible, centered
    void actualPixels();  // 100% zoom, document centered
    void zoomIn();        // step zoom in about the viewport center
    void zoomOut();       // step zoom out about the viewport center
    [[nodiscard]] double zoomPercent() const noexcept { return view_.zoom() * 100.0; }

    // DocumentObserver: re-flatten and repaint after any committed change.
    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    [[nodiscard]] QSize sizeHint() const override;
    void tabletEvent(QTabletEvent*) override;

private:
    void refreshImage();                   // re-flatten doc_ -> image_
    void zoomAroundCenter(double factor);  // zoom about the viewport center
    void maybeInitialFit();                // fit once the widget has a real size
    [[nodiscard]] pe::StrokePoint sampleAt(QPointF widgetPos) const;  // widget -> doc space

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    std::unique_ptr<pe::CanvasRenderer> renderer_;  // owns dirty-tile cache for efficient updates
    std::unique_ptr<pe::RHIDevice> rhi_;  // software RHI for display (task next)
    QImage image_;                 // a private copy of the current composite (RGBA8888)
    pe::PaintToolController tool_;
    pe::ViewTransform view_;  // document <-> widget (device px) mapping
    QBrush checker_;          // transparency checkerboard (device-space tile)
    Tool toolMode_ = Tool::Brush;

    bool needsFit_ = true;  // fit-to-window pending until the widget has a valid size
    bool panning_ = false;  // pan in progress (middle-drag, or Hand tool + left-drag)
    QPointF lastPanPos_;    // last pan sample (widget space)

    // Marquee selection drag state (live rect in document pixels)
    bool draggingMarquee_ = false;
    QPointF marqueeAnchor_;   // widget space start of drag
    Rect liveMarquee_{};      // current doc-space rect (normalized)

    // Move tool drag state
    bool draggingMove_ = false;
    QPointF moveAnchor_;
    pe::PointD moveStartDoc_{};
};

}  // namespace pe::app
