#pragma once

#include "pe/core/Document.hpp"
#include "pe/core/PaintToolController.hpp"
#include "pe/core/ViewTransform.hpp"

#include <memory>
#include <vector>

#include <QBrush>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>

namespace pe {
class CanvasRenderer;  // tile-cache renderer; defined in pe/core/CanvasRenderer.hpp
}

namespace pe::app {

// The central canvas: composites the visible document region through the engine's
// CanvasRenderer tile cache and presents it through a zoom/pan view transform, routing
// mouse input to the tools. It observes the document, so committed edits (paint commits,
// undo/redo, file loads) repaint automatically; live previews repaint explicitly and
// invalidate the cache as they are drawn.
//
// Painting only the visible tiles means even a canvas larger than the whole-image composite
// budget renders, and pan/zoom recomposites just the newly-exposed tiles. View navigation:
// mouse wheel zooms about the cursor, middle-drag pans, the document fits on first show. The
// presentation transform is the headless pe::ViewTransform (docs/systems/02-canvas-rendering);
// the document is never resampled. GPU display arrives later.
class CanvasView : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    // How the canvas interprets a left-button gesture. Brush/Eraser paint, Hand
    // pans, Zoom clicks to zoom; Inactive is a selected-but-unimplemented tool
    // (clicks do nothing) — the scaffold the rest of the toolset wires into.
    enum class Tool {
        Brush,
        Eraser,
        Hand,
        Zoom,
        Move,
        Marquee,
        Lasso,
        Wand,
        Crop,
        Bucket,
        Gradient,
        Type,
        Dodge,  // tone brush (lighten); Alt during the stroke burns (darkens) instead
        Eyedropper,
        Inactive
    };

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

    // Background color (the Gradient tool's far stop; the foreground is the paint color).
    void setBackgroundColor(const QColor& c) { bgColor_ = c; }

signals:
    void zoomChanged(double percent);   // for the status-bar zoom readout
    void colorPicked(const QColor& c);  // for eyedropper tool
    void toolMessage(
        const QString& msg);  // transient status-bar feedback (e.g. a fill that no-ops)
    void textRequested(const QPointF& docPos);  // Type tool clicked at this doc-space point

public:
    // View navigation (also driven by the View menu).
    void fitToWindow();   // scale so the whole document is visible, centered
    void actualPixels();  // 100% zoom, document centered
    void zoomIn();        // step zoom in about the viewport center
    void zoomOut();       // step zoom out about the viewport center
    [[nodiscard]] double zoomPercent() const noexcept { return view_.zoom() * 100.0; }

    // DocumentObserver: re-flatten and repaint after any committed change.
    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

    // Drop the renderer's cached tiles and repaint. For external edits that mutate the
    // document outside the observer/command flow — e.g. an effect dialog's or the move
    // tool's live preview, which applies a provisional command directly (no notification),
    // so the tile cache would otherwise show stale pixels until the next committed change.
    void reloadImage();

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
    void zoomAroundCenter(double factor);  // zoom about the viewport center
    void maybeInitialFit();                // fit once the widget has a real size
    [[nodiscard]] pe::StrokePoint sampleAt(QPointF widgetPos) const;  // widget -> doc space
    [[nodiscard]] pe::Size canvasSize() const;  // doc_'s canvas size, or {0,0} if no document
    // Why a Bucket/Gradient fill returned no command, for status-bar feedback: the active layer
    // isn't paintable pixels, or the canvas exceeds the engine's per-op fill budget.
    [[nodiscard]] QString fillUnavailableMessage() const;

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    // Tile-cache renderer bound to doc_: composites only visible/dirty tiles, so a huge
    // canvas no longer overflows the whole-image composite budget and pan/zoom stays cheap.
    std::unique_ptr<pe::CanvasRenderer> renderer_;
    pe::PaintToolController tool_;
    pe::ViewTransform view_;  // document <-> widget (device px) mapping
    QBrush checker_;          // transparency checkerboard (device-space tile)
    Tool toolMode_ = Tool::Brush;

    bool needsFit_ = true;  // fit-to-window pending until the widget has a valid size
    bool panning_ = false;  // pan in progress (middle-drag, or Hand tool + left-drag)
    QPointF lastPanPos_;    // last pan sample (widget space)

    // Marquee selection drag state (live rect in document pixels)
    bool draggingMarquee_ = false;
    QPointF marqueeAnchor_;  // widget space start of drag
    Rect liveMarquee_{};     // current doc-space rect (normalized)

    // Lasso freehand selection: the in-progress polygon in document pixels (rasterized into
    // a polygon selection on release).
    bool draggingLasso_ = false;
    std::vector<pe::Point> lassoPts_;

    // Gradient tool drag: a guide line (widget space) drawn while dragging; on release the
    // foreground->background gradient is applied along start->end.
    bool draggingGradient_ = false;
    QPointF gradStartWidget_;
    QPointF gradEndWidget_;
    QColor bgColor_{255, 255, 255};  // gradient far stop (kept in sync by MainWindow)

    // Pixel-tight bounds of the committed selection, for the marching-ants outline. Cached
    // on selection change (and on setDocument) so paintEvent never scans the mask per frame.
    Rect selectionAnts_{};

    // Move-tool drag state: a live preview shifts the active layer's content by the drag
    // delta (a provisional command reverted on each move and committed on release).
    void cancelMovePreview();  // revert + drop any in-progress move preview
    bool movingContent_ = false;
    QPointF moveStartWidget_;               // drag start (widget space)
    pe::LayerId moveLayer_ = pe::kNoLayer;  // the layer captured at drag start
    std::unique_ptr<pe::PaintCommand> movePreview_;
};

}  // namespace pe::app
