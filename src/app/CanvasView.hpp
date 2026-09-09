#pragma once

#include "BusyTask.hpp"

#include "pe/core/Channels.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PaintToolController.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/ViewTransform.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <QBrush>
#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QVector>
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
        Dodge,      // tone brush (lighten); Alt during the stroke burns (darkens) instead
        Clone,      // clone stamp; Alt-click sets the source, then drag clones from it
        Blur,       // blur brush; drag to locally soften existing pixels under the stroke
        Heal,       // spot healing brush; drag over a blemish to dissolve it into its surroundings
        Transform,  // free transform: scale (corners) / rotate (top handle) / move the active layer
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

    // Move tool options (#134). Both default off, which is the behaviour that shipped.
    //
    // Auto-Select picks the layer under the cursor when a Move drag starts, instead of
    // moving whatever happens to be active. Its granularity is the individual layer, or the
    // top-level group containing it.
    enum class AutoSelectMode { Layer, Group };
    void setAutoSelect(bool on) noexcept { autoSelect_ = on; }
    void setAutoSelectMode(AutoSelectMode m) noexcept { autoSelectMode_ = m; }
    [[nodiscard]] bool autoSelect() const noexcept { return autoSelect_; }
    [[nodiscard]] AutoSelectMode autoSelectMode() const noexcept { return autoSelectMode_; }

    // Show Transform Controls draws the active layer's bounding box and handles under the
    // Move tool. Grabbing a corner or the rotate knob enters Free Transform, which is what
    // the box is for; a press anywhere else is still a move.
    void setShowTransformControls(bool on);
    [[nodiscard]] bool showTransformControls() const noexcept { return showTransformControls_; }
    [[nodiscard]] Tool activeTool() const noexcept { return toolMode_; }

    // True while a Free Transform session is live (an uncommitted preview is applied directly to
    // the tiles, outside history). MainWindow gates undo/redo on this — mutating history underneath
    // the preview would desync its tile snapshots (same reason as tool().isStroking()).
    [[nodiscard]] bool isTransforming() const noexcept { return transforming_; }

    // Background color (the Gradient tool's far stop; the foreground is the paint color).
    void setBackgroundColor(const QColor& c) { bgColor_ = c; }

    // Magic Wand per-channel tolerance (clamped to [0,255]); driven by the options bar.
    void setWandTolerance(int t) { wandTolerance_ = std::clamp(t, 0, 255); }

    // Route the Brush into the active layer's MASK instead of its pixels (set by clicking a mask
    // thumbnail in the Layers panel). The foreground luminance drives it: black hides, white
    // reveals. Cleared when the target layer/mask goes away or the document changes.
    void setMaskEditTarget(bool on);
    [[nodiscard]] bool maskEditTarget() const noexcept { return maskEditTarget_; }

    // Which colour channels the canvas draws (the Channels panel). Display state only: it
    // changes what is painted and nothing about the document, so it survives undo, is not
    // undoable itself, and never marks the document dirty.
    void setChannelView(pe::ChannelView v);
    [[nodiscard]] pe::ChannelView channelView() const noexcept { return channelView_; }

    // A bounded, downscaled composite of the whole canvas, at most `maxPixels` of output.
    // For panel thumbnails: it goes through the renderer's tile cache and its scaled path, so
    // the cost is set by `maxPixels` rather than by the document, and unlike
    // Document::compositeImage() it keeps working above the composite cap.
    [[nodiscard]] pe::PixelBuffer canvasPreview(int maxPixels);

signals:
    void zoomChanged(double percent);   // for the status-bar zoom readout
    void colorPicked(const QColor& c);  // for eyedropper tool
    void toolMessage(
        const QString& msg);  // transient status-bar feedback (e.g. a fill that no-ops)
    void textRequested(const QPointF& docPos);  // Type tool clicked at this doc-space point
    // Live pointer position in DOCUMENT pixels, for the status-bar readout. Emitted on
    // every move (mouse tracking is on), including mid-drag. Document space rather than
    // widget space, because the widget position is meaningless at any zoom but 100%.
    void cursorMoved(const QPointF& docPos);
    void cursorLeft();  // pointer left the canvas; the readout should stop showing a stale point
    // Mask-edit was exited because a non-Brush tool became active (only the Brush paints masks).
    // MainWindow relays it so the Layers panel drops the focus ring; keeps the ring honest.
    void maskEditTargetCleared();
    // The brush settings changed because the TOOL changed, so whatever is showing them has
    // to catch up. Not emitted when the settings are edited through those controls, which
    // would echo straight back at them.
    void brushSettingsSwapped();
    // The canvas changed the active tool ITSELF: grabbing a transform handle under the Move
    // tool enters Free Transform. Without this the tool strip would keep claiming Move while
    // the canvas was transforming. Not emitted when the shell set the tool.
    void toolChanged(Tool t);
    // A canvas gesture declined for a structural reason. Distinct from toolMessage, which
    // is a transient hint: this is the answer to "why did nothing happen", and MainWindow
    // records it as well as showing it.
    void refused(const pe::Refusal& r);

public:
    // View navigation (also driven by the View menu).
    void fitToWindow();   // scale so the whole document is visible, centered
    void actualPixels();  // 100% zoom, document centered
    void zoomIn();        // step zoom in about the viewport center
    void zoomOut();       // step zoom out about the viewport center
    [[nodiscard]] double zoomPercent() const noexcept { return view_.zoom() * 100.0; }

    // Document point -> widget point under the current view transform. Public so a caller
    // that means "the pixel at (30, 30)" can say where that is on screen: deriving it from
    // the zoom and the centring is how a test ends up passing for the wrong reason.
    [[nodiscard]] QPointF docToWidget(pe::PointD docPos) const;

    // The document-space rect currently on screen, padded. Public because it is what bounds
    // an interactive preview to work the user can actually see, and a test needs to be able
    // to say what that region is.
    [[nodiscard]] pe::Rect visibleDocRect() const;

    // The committed selection's outline as the view draws it, in document coordinates.
    // Public because "the ants follow the selection and not its bounding box" is the
    // property that broke, and it cannot be asserted from outside without seeing them.
    [[nodiscard]] const QVector<QLineF>& selectionOutline() const noexcept {
        return selectionAnts_;
    }

    // DocumentObserver: re-flatten and repaint after any committed change.
    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

    // Drop the renderer's cached tiles and repaint. For external edits that mutate the
    // document outside the observer/command flow — e.g. an effect dialog's or the move
    // tool's live preview, which applies a provisional command directly (no notification),
    // so the tile cache would otherwise show stale pixels until the next committed change.
    void reloadImage();

    // The bounded sibling of reloadImage(). A preview applied without notifying leaves the
    // renderer cache stale, but only over the rect the command reports, and dropping the
    // WHOLE cache on every mouse-move of a drag cost a full recomposite per event.
    //
    // Safe because the compositor computes each display tile purely from that tile's own
    // source pixels, so a cached tile outside `docRect` still equals a fresh recomposite.
    // The rect is the one PaintCommand returns, which over-covers (it unions whole tile
    // slices) and can never under-cover, because a command writes only inside it.
    void repaintRegion(pe::Rect docRect);

    // Diagnostics and tests: the tile-cache renderer bound to the current document, or null
    // when there is none. Exposed because there is otherwise no way for a test to tell a
    // bounded invalidation from an unbounded one.
    [[nodiscard]] pe::CanvasRenderer* renderer() noexcept { return renderer_.get(); }

    // Stop reading the document to paint, and show the last frame instead.
    //
    // A background task (a save, an export, a wand click) runs the engine on a worker
    // thread, and the engine is single threaded: tile stores and the renderer hold mutable
    // caches, so a repaint compositing from this thread at the same time is a data race,
    // not merely a stale picture. Freezing removes the paint path as a second reader. The
    // frame is grabbed at the moment of freezing, so what stays on screen is exactly what
    // the user was looking at when the task started.
    //
    // Paired with the input block in BusyTask.hpp, which stops every other handler here
    // from being entered. Prefer runDocumentTask over calling this directly.
    void setFrozen(bool on);
    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    // How this view runs a long operation.
    //
    // The canvas has long tools of its own (the Magic Wand samples and flood-fills a whole
    // composite), and they used to call runDocumentTask directly. That skipped the window's
    // re-entrancy guard entirely: a wand click during a snapshot save, which deliberately
    // leaves the canvas live, started a second worker and a second nested event loop inside
    // the first, and left the window's close guard inert for the wand's whole duration.
    //
    // MainWindow installs its guarded runner here, so the canvas takes the same guard as a
    // File action. Left unset the view runs the work itself, which keeps CanvasView usable
    // on its own and in tests that construct no window.
    using TaskRunner =
        std::function<TaskResult(const QString&, TaskAccess, const std::function<void()>&)>;
    void setTaskRunner(TaskRunner runner) { taskRunner_ = std::move(runner); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    [[nodiscard]] QSize sizeHint() const override;
    void tabletEvent(QTabletEvent*) override;
    void keyPressEvent(QKeyEvent*) override;  // Enter commits / Esc cancels a live transform

private:
    void zoomAroundCenter(double factor);  // zoom about the viewport center
    void maybeInitialFit();                // fit once the widget has a real size
    [[nodiscard]] pe::StrokePoint sampleAt(QPointF widgetPos) const;  // widget -> doc space
    // Run `work` through the installed TaskRunner, or directly if none is installed. Every
    // long operation this view starts goes through here, so the window's guard cannot be
    // skipped by adding a second tool later.
    [[nodiscard]] TaskResult runCanvasTask(const QString& title, TaskAccess access,
                                           const std::function<void()>& work);

    [[nodiscard]] pe::Size canvasSize() const;  // doc_'s canvas size, or {0,0} if no document
    // Why a Bucket/Gradient fill returned no command, for status-bar feedback: the active layer
    // isn't paintable pixels, or the canvas exceeds the engine's per-op fill budget.
    [[nodiscard]] QString fillUnavailableMessage() const;
    // Why PaintToolController::begin() refused, phrased for the status bar. A stroke
    // that cannot start used to do nothing and say nothing, which is the same thing a
    // broken brush looks like.
    [[nodiscard]] QString paintUnavailableMessage() const;
    // Why a stroke stopped following the cursor: it outgrew the bake budget of the engine
    // behind the active tool and was frozen at its last representable state.
    [[nodiscard]] QString strokeAtBudgetMessage() const;
    // Clone-tool press handling shared by the mouse and tablet paths. Alt-click sets the source
    // anchor (`docPt` in document space); a click with no source set shows a hint. Returns true if
    // the press was consumed (no stroke should begin), false to begin a clone stroke.
    [[nodiscard]] bool handleClonePress(const pe::PointD& docPt, bool altHeld);

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    // Tile-cache renderer bound to doc_: composites only visible/dirty tiles, so a huge
    // canvas no longer overflows the whole-image composite budget and pan/zoom stays cheap.
    std::unique_ptr<pe::CanvasRenderer> renderer_;
    pe::PaintToolController tool_;
    pe::ViewTransform view_;  // document <-> widget (device px) mapping
    QBrush checker_;          // transparency checkerboard (device-space tile)
    Tool toolMode_ = Tool::Brush;
    bool maskEditTarget_ =
        false;  // Brush paints the active layer's mask (set via the Layers panel)

    // While a worker owns the document, paint this instead of compositing. See setFrozen.
    bool frozen_ = false;
    TaskRunner taskRunner_;  // unset: run the work directly (see setTaskRunner)
    // Whether this drag has already explained why the move is being refused. Reset on each
    // press, so the reason is said once rather than on every motion event.
    bool moveRefusalSaid_ = false;
    bool autoSelect_ = false;
    AutoSelectMode autoSelectMode_ = AutoSelectMode::Layer;
    bool showTransformControls_ = false;

    // Display state, not document state: which colour channels get drawn. Default is the
    // composite, so a canvas nobody has touched the Channels panel on pays nothing.
    pe::ChannelView channelView_{};

    // Brush settings, kept per paint mode.
    //
    // One shared set is how you drop the brush to 30% for a soft pass, reach for the Eraser,
    // and find it erasing at 30% too, with nothing on screen saying why: the options bar
    // shows the number, but the user is not thinking of it as the eraser's number. Every
    // comparable editor keeps them per tool.
    //
    // MaskPaint deliberately shares the Brush's slot: painting a mask is still the Brush,
    // and a size that changes when a mask thumbnail is clicked would be its own surprise.
    static constexpr std::size_t kPaintModeCount = 9;
    [[nodiscard]] static std::size_t brushSlotFor(pe::PaintToolController::Mode m) noexcept;
    std::array<pe::BrushSettings, kPaintModeCount> brushPerMode_{};
    pe::PaintToolController::Mode brushSlotMode_ = pe::PaintToolController::Mode::Brush;
    void swapBrushSettingsTo(pe::PaintToolController::Mode m);
    QPixmap frozenFrame_;

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
    int wandTolerance_ = 32;  // magic-wand per-channel tolerance (kept in sync by MainWindow)

    // Pixel-tight bounds of the committed selection, for the marching-ants outline. Cached
    // on selection change (and on setDocument) so paintEvent never scans the mask per frame.
    // The committed selection's boundary, in DOCUMENT coordinates, rebuilt on each selection
    // change so paintEvent never traces the mask per frame. Line segments rather than a rect:
    // drawing tightBounds() made every lasso and every wand look like it had snapped to a box.
    QVector<QLineF> selectionAnts_;
    // The bounds to fall back to when the boundary was too ragged to trace, and whether the
    // user has been told. Said once per selection, not once per repaint.
    Rect selectionAntsBounds_{};
    bool selectionAntsComplete_ = true;
    void rebuildSelectionAnts();

    // Move-tool drag state: a live preview shifts the active layer's content by the drag
    // delta (a provisional command reverted on each move and committed on release).
    // Revert and drop any in-progress move preview. Returns the rect the revert dirtied so
    // the caller can bound its repaint; empty when there was no preview.
    [[nodiscard]] pe::Rect cancelMovePreview();
    bool movingContent_ = false;
    QPointF moveStartWidget_;               // drag start (widget space)
    pe::LayerId moveLayer_ = pe::kNoLayer;  // the layer captured at drag start
    std::unique_ptr<pe::PaintCommand> movePreview_;

    // Free Transform session: a live affine (uniform scale about the box center, rotation about the
    // center, and translation) of the active pixel layer's content. Previewed as a provisional
    // command (reverted + reapplied from the original content each gesture) and committed on
    // Enter / click-outside; Esc cancels. transformBox_ is the ORIGINAL content bounds, fixed for
    // the session; the params below place it.
    void beginTransform();          // start a session on the active pixel layer (no-op otherwise)
    void updateTransformPreview();  // reapply transformLayerContent(matrix) from the original
    void commitTransform();         // push the final command (one undo step) and end the session
    void cancelTransform();         // revert the preview and end the session
    [[nodiscard]] pe::Affine2D transformMatrix() const;   // original-box -> current placement
    [[nodiscard]] pe::PointD transformCenterDoc() const;  // current box center (doc space)

    // Which box the on-canvas handles refer to, and the matrix placing it. Two callers show
    // them: a live Free Transform session, and the Move tool with Show Transform Controls,
    // where the box is the active layer's bounds with nothing applied yet. Returns false
    // when there are no controls to show.
    [[nodiscard]] bool controlsGeometry(pe::Rect& box, pe::Affine2D& m) const;

    // The handle positions for one box, in widget space. Kept in one place because the
    // painter and the hit test have to agree about where a handle is to within a few
    // pixels, and they used to compute it twice from the same fields.
    struct ControlPoints {
        QPointF corner[4];  // TL, TR, BR, BL
        QPointF rotate;     // the knob out past the top edge
    };
    [[nodiscard]] ControlPoints controlPointsFor(pe::Rect box, const pe::Affine2D& m) const;

    // Hit-test a widget point: 0..3 = corner (scale), 4 = rotate handle, 5 = inside (move), -1
    // none.
    [[nodiscard]] int hitTransformHandle(QPointF widgetPos) const;

    bool transforming_ = false;
    pe::LayerId transformLayer_ = pe::kNoLayer;
    pe::Rect transformBox_{};  // original content bounds (doc space), fixed for the session
    double tfScale_ = 1.0;     // uniform scale about the box center
    double tfAngle_ = 0.0;     // rotation (radians) about the box center
    pe::PointD tfTranslate_{0.0, 0.0};  // extra translation (doc space)
    int tfDrag_ = -1;                   // active handle during a drag (-1 = none)
    pe::PointD tfDragStartDoc_{0.0, 0.0};
    double tfDragStartScale_ = 1.0;
    double tfDragStartAngle_ = 0.0;
    pe::PointD tfDragStartTranslate_{0.0, 0.0};
    std::unique_ptr<pe::PaintCommand> transformPreview_;
};

}  // namespace pe::app
