#include "CanvasView.hpp"

#include "BusyTask.hpp"
#include "Theme.hpp"
#include "pe/core/Brush.hpp"  // pe::PaintCommand (move-tool preview command)
#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"  // kMaxCompositeImagePixels (extreme-zoom-out downscale threshold)
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/HitTest.hpp"  // pe::moveLayerContent
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Selection.hpp"

#include <QApplication>
#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTabletEvent>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pe::app {

namespace {
constexpr double kZoomStep = 1.25;  // per Zoom In/Out and per wheel notch

// How far the rotate knob sits out past the top edge of the transform box, in widget
// pixels. Named because the painter and the hit test both need it and a disagreement here
// draws the knob somewhere the user cannot grab it.
constexpr double kRotateHandleOffsetPx = 24.0;

// Map an engine Affine2 to a QTransform. Both map (x,y) -> (a*x + c*y + e,
// b*x + d*y + f), so the six coefficients line up directly.
[[nodiscard]] QTransform toQTransform(const pe::Affine2& m) {
    return QTransform(m.a, m.b, m.c, m.d, m.e, m.f);
}

// A 16px light/white transparency checkerboard tile (device space).
[[nodiscard]] QBrush makeCheckerBrush() {
    constexpr int kCell = 8;
    QPixmap tile(kCell * 2, kCell * 2);
    tile.fill(QColor(255, 255, 255));
    QPainter p(&tile);
    const QColor gray(200, 200, 200);
    p.fillRect(0, 0, kCell, kCell, gray);
    p.fillRect(kCell, kCell, kCell, kCell, gray);
    return QBrush(tile);
}
}  // namespace

CanvasView::CanvasView(QWidget* parent) : QWidget(parent), checker_(makeCheckerBrush()) {
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);  // so Free Transform receives Enter (commit) / Esc (cancel)
    // Report the pointer position even with no button held, for the status-bar readout.
    // Every branch of mouseMoveEvent is gated on a drag flag, so button-less moves fall
    // through to the base class and do no work.
    setMouseTracking(true);
    // A visible default: an opaque black, medium round tip.
    pe::BrushSettings b = tool_.brush();
    b.diameter = 24.0f;
    tool_.setBrush(b);
}

CanvasView::~CanvasView() {
    renderer_.reset();  // unregister the renderer's observer before doc_ goes away
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void CanvasView::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) {
        // Abandon any in-progress stroke or move on the outgoing document, reverting its
        // live preview, so a tool never carries provisional state across documents.
        if (tool_.isStroking()) tool_.cancel(*doc_);
        // The rect is deliberately discarded here: the renderer is destroyed two lines
        // below, so there is no cache left to invalidate.
        (void)cancelMovePreview();
        cancelTransform();  // revert any live transform preview before detaching
        renderer_.reset();  // unregister the old renderer before detaching this view
        doc_->removeObserver(this);
    }
    // Drop any in-progress selection drag so a release after the swap can't commit a
    // stale gesture (old-document coordinates) against the new document.
    draggingMarquee_ = false;
    liveMarquee_ = Rect{};
    draggingLasso_ = false;
    lassoPts_.clear();
    draggingGradient_ = false;
    tool_.clearCloneSource();  // a clone anchor is stale doc-space state across documents
    // The mask-edit target belonged to the outgoing document; reset so the Brush paints pixels
    // again until the user targets a mask in the new document.
    maskEditTarget_ = false;
    if (toolMode_ == Tool::Brush) tool_.setMode(pe::PaintToolController::Mode::Brush);
    doc_ = doc;
    if (doc_ != nullptr) {
        doc_->addObserver(this);
        renderer_ = std::make_unique<pe::CanvasRenderer>(*doc_);  // observes doc_ itself
    }
    rebuildSelectionAnts();
    needsFit_ = true;  // fit the new document once we have a valid widget size
    maybeInitialFit();
    updateGeometry();
    update();
}

void CanvasView::rebuildSelectionAnts() {
    selectionAnts_.clear();
    selectionAntsBounds_ = pe::Rect{};
    const bool wasComplete = selectionAntsComplete_;
    selectionAntsComplete_ = true;
    if (doc_ == nullptr) return;

    const pe::Selection& sel = doc_->selection();
    const pe::SelectionOutline traced = sel.outline();
    selectionAntsComplete_ = traced.complete;
    if (!traced.complete) {
        // Too ragged to draw every frame. The bounds are at least true as a BOUND, but they
        // are not the selection, so the user is told rather than left to infer it from a
        // rectangle that paints like something else. Once per selection, not per repaint.
        selectionAntsBounds_ = sel.tightBounds();
        if (wasComplete) {
            emit toolMessage(
                QStringLiteral("Selection outline is too detailed to draw; showing its bounds. "
                               "The selection itself is unaffected."));
        }
        return;
    }
    selectionAnts_.reserve(static_cast<qsizetype>(traced.segments.size()));
    for (const pe::OutlineSegment& s : traced.segments) {
        selectionAnts_.append(QLineF(s.a.x, s.a.y, s.b.x, s.b.y));
    }
}

void CanvasView::onDocumentChanged(const pe::Document&, const pe::DocumentChange& ch) {
    if (ch.kind == pe::DocumentChange::Kind::Selection) {
        // Selection change only affects the marching-ants overlay, not pixel content.
        // Retrace the outline here, once per change, so paintEvent never scans the mask.
        rebuildSelectionAnts();
        update();
        return;
    }
    // An EXTERNAL committed mutation while a Free Transform session is live (undo/redo, a
    // layers-panel edit, a file load) invalidates the session — the layer it captured may be gone
    // or changed. End it (reverting its provisional preview) before repainting. Self-induced
    // commits don't reach here with transforming_ still true (commitTransform clears it before
    // pushing).
    if (transforming_) cancelTransform();
    // A committed mutation (paint commit, undo/redo, file load). The renderer is also an
    // observer and has already marked the changed tiles dirty, so we only need to repaint;
    // paintEvent recomposites just those tiles. The live brush preview repaints separately.
    update();
}

void CanvasView::repaintRegion(pe::Rect docRect) {
    if (renderer_ != nullptr && !docRect.isEmpty()) renderer_->invalidate(docRect);
    update();
}

void CanvasView::setChannelView(pe::ChannelView v) {
    if (v == channelView_) return;
    channelView_ = v;
    // The renderer's tiles are untouched: what changed is how they are drawn, so a repaint
    // is the whole of it. Invalidating the cache here would recomposite the canvas every
    // time an eye was clicked, for no difference in the pixels.
    update();
}

pe::PixelBuffer CanvasView::canvasPreview(int maxPixels) {
    // Frozen means a worker owns the document; compositing here would race it. The caller
    // gets an empty buffer and keeps whatever it drew last, which is what paintEvent does.
    if (frozen_ || doc_ == nullptr || renderer_ == nullptr) return pe::PixelBuffer{};
    const pe::Size cs = canvasSize();
    if (cs.isEmpty()) return pe::PixelBuffer{};
    return renderer_->renderRegionScaled(pe::Rect{0, 0, cs.width, cs.height},
                                         std::max(1, maxPixels));
}

void CanvasView::reloadImage() {
    // A live preview applied a provisional command straight to the document (no
    // notification), so the renderer's cache is stale — drop it and repaint.
    if (renderer_ != nullptr) renderer_->invalidateAll();
    update();
}

pe::Size CanvasView::canvasSize() const {
    return doc_ != nullptr ? doc_->canvasSize() : pe::Size{0, 0};
}

QString CanvasView::fillUnavailableMessage() const {
    // A fill (Bucket/Gradient) returned no command. The two reasons the app can distinguish: the
    // active layer is not a paintable pixel layer, otherwise the canvas is over the engine's
    // per-operation fill budget (the fills always span the whole canvas).
    const pe::Layer* layer = doc_ != nullptr ? doc_->findLayer(doc_->activeLayer()) : nullptr;
    if (layer == nullptr || layer->kind() != pe::LayerKind::Pixel) {
        return QStringLiteral("Select a pixel layer to fill.");
    }
    return QStringLiteral("Image is too large to fill in one step.");
}

QString CanvasView::paintUnavailableMessage() const {
    // Mirrors the conditions PaintToolController::begin() rejects on: mask painting
    // needs the active layer to carry a mask, everything else needs a pixel layer.
    const pe::Layer* layer = doc_ != nullptr ? doc_->findLayer(doc_->activeLayer()) : nullptr;
    if (layer == nullptr) return QStringLiteral("Select a layer to paint on.");
    if (tool_.mode() == pe::PaintToolController::Mode::MaskPaint) {
        return layer->mask() == nullptr ? QStringLiteral("This layer has no mask to paint on.")
                                        : QStringLiteral("Cannot paint on this layer.");
    }
    if (layer->kind() != pe::LayerKind::Pixel) {
        return QStringLiteral("Select a pixel layer to paint on.");
    }
    return QStringLiteral("Cannot paint on this layer.");
}

QString CanvasView::strokeAtBudgetMessage() const {
    // Names the tool, because the budget differs per engine and the user's next move is
    // to shorten the stroke for that specific tool.
    switch (tool_.mode()) {
        case pe::PaintToolController::Mode::Heal:
            return QStringLiteral(
                "Healing stopped: that stroke covers too large an area. "
                "Release and heal in shorter strokes.");
        case pe::PaintToolController::Mode::Blur:
        case pe::PaintToolController::Mode::Sharpen:
            return QStringLiteral(
                "Stroke stopped: it covers too large an area. "
                "Release and work in shorter strokes.");
        case pe::PaintToolController::Mode::MaskPaint:
            return QStringLiteral(
                "Mask stroke stopped: it covers too large an area. "
                "Release and paint in shorter strokes.");
        default:
            return QStringLiteral("Stroke stopped: it covers too large an area.");
    }
}

bool CanvasView::handleClonePress(const pe::PointD& docPt, bool altHeld) {
    if (altHeld) {
        // Alt-click sets the clone source anchor (no stroke); the next drag clones from it.
        tool_.setCloneSource(pe::Point{static_cast<int>(std::lround(docPt.x)),
                                       static_cast<int>(std::lround(docPt.y))});
        emit toolMessage(QStringLiteral("Clone source set"));
        return true;
    }
    if (!tool_.hasCloneSource()) {
        emit toolMessage(QStringLiteral("Alt-click to set a clone source first."));
        return true;
    }
    return false;  // begin a clone stroke
}

pe::Rect CanvasView::cancelMovePreview() {
    pe::Rect dirty{};
    // Captured BEFORE the reset: PaintCommand exposes no rect accessor, so once the pointer
    // is gone the rect is unrecoverable and the caller has nothing left to invalidate.
    if (movePreview_ && doc_ != nullptr) dirty = movePreview_->undo(*doc_).dirtyRegion;
    movePreview_.reset();
    movingContent_ = false;
    moveLayer_ = pe::kNoLayer;
    return dirty;
}

QSize CanvasView::sizeHint() const {
    const pe::Size cs = canvasSize();
    return cs.isEmpty() ? QSize(640, 480) : QSize(cs.width, cs.height);
}

void CanvasView::fitToWindow() {
    if (canvasSize().isEmpty()) return;
    const double w = canvasSize().width;
    const double h = canvasSize().height;
    if (w <= 0.0 || h <= 0.0) return;
    const double margin = 0.96;  // a little breathing room around the image
    const double z = std::min(static_cast<double>(width()) / w, static_cast<double>(height()) / h);
    view_.setRotation(0.0);
    view_.setZoom(z * margin);
    view_.setFocus(pe::PointD{w / 2.0, h / 2.0},
                   pe::PointD{width() / 2.0, height() / 2.0});  // doc center -> viewport center
    update();
    emit zoomChanged(zoomPercent());
}

void CanvasView::actualPixels() {
    if (canvasSize().isEmpty()) return;
    needsFit_ = false;
    view_.setRotation(0.0);
    view_.setZoom(1.0);
    view_.setFocus(pe::PointD{canvasSize().width / 2.0, canvasSize().height / 2.0},
                   pe::PointD{width() / 2.0, height() / 2.0});
    update();
    emit zoomChanged(zoomPercent());
}

void CanvasView::zoomAroundCenter(double factor) {
    if (doc_ == nullptr) return;
    needsFit_ = false;
    const double target = std::clamp(view_.zoom() * factor, pe::kMinZoom, pe::kMaxZoom);
    view_.zoomAround(pe::PointD{width() / 2.0, height() / 2.0}, target);
    update();
    emit zoomChanged(zoomPercent());
}

void CanvasView::zoomIn() {
    zoomAroundCenter(kZoomStep);
}

void CanvasView::zoomOut() {
    zoomAroundCenter(1.0 / kZoomStep);
}

void CanvasView::setTool(Tool t) {
    const bool changed = toolMode_ != t;
    toolMode_ = t;
    // Abandon any live stroke FIRST, before changing the paint mode: otherwise a tool change
    // mid-stroke (e.g. a shortcut while the button is held) would leave the stroke active and the
    // next mouse-move would rebuild it under the new mode — silently changing what it commits.
    if (doc_ != nullptr && tool_.isStroking()) {
        tool_.cancel(*doc_);
        reloadImage();  // the cancel reverted tiles without notifying the renderer
    }
    // Only the Brush paints masks, so selecting any other tool exits mask-edit — otherwise the
    // Layers-panel focus ring would lie while the new tool edits pixels. Tell the panel to drop it.
    if (maskEditTarget_ && t != Tool::Brush) {
        maskEditTarget_ = false;
        emit maskEditTargetCleared();
    }
    // Carry the current settings back to the outgoing tool's slot and load the incoming
    // one's, BEFORE the mode changes, so a stroke never straddles two sets.
    if (t == Tool::Brush) {
        swapBrushSettingsTo(maskEditTarget_ ? pe::PaintToolController::Mode::MaskPaint
                                            : pe::PaintToolController::Mode::Brush);
    } else if (t == Tool::Eraser) {
        swapBrushSettingsTo(pe::PaintToolController::Mode::Eraser);
    } else if (t == Tool::Dodge) {
        swapBrushSettingsTo(pe::PaintToolController::Mode::Dodge);
    } else if (t == Tool::Clone) {
        swapBrushSettingsTo(pe::PaintToolController::Mode::Clone);
    } else if (t == Tool::Blur) {
        swapBrushSettingsTo(pe::PaintToolController::Mode::Blur);
    } else if (t == Tool::Heal) {
        swapBrushSettingsTo(pe::PaintToolController::Mode::Heal);
    }

    if (t == Tool::Brush) {
        // When a layer mask is the edit target, the Brush paints the mask instead of pixels.
        tool_.setMode(maskEditTarget_ ? pe::PaintToolController::Mode::MaskPaint
                                      : pe::PaintToolController::Mode::Brush);
    } else if (t == Tool::Eraser) {
        tool_.setMode(pe::PaintToolController::Mode::Eraser);
    } else if (t == Tool::Dodge) {
        tool_.setMode(pe::PaintToolController::Mode::Dodge);  // Alt at stroke start burns instead
    } else if (t == Tool::Clone) {
        tool_.setMode(pe::PaintToolController::Mode::Clone);  // Alt-click sets the source anchor
    } else if (t == Tool::Blur) {
        tool_.setMode(pe::PaintToolController::Mode::Blur);  // drag to locally soften pixels
    } else if (t == Tool::Heal) {
        tool_.setMode(pe::PaintToolController::Mode::Heal);  // drag over a blemish to heal it
    }
    if (draggingMarquee_ && doc_ != nullptr) {
        draggingMarquee_ = false;
        liveMarquee_ = Rect{};
        update();
    }
    if (movingContent_) {  // switching away from Move drops any live move preview
        repaintRegion(cancelMovePreview());
    }
    if (draggingLasso_) {  // switching away mid-lasso discards the in-progress path
        draggingLasso_ = false;
        lassoPts_.clear();
        update();
    }
    if (draggingGradient_) {  // switching away mid-gradient discards the in-progress guide
        draggingGradient_ = false;
        update();
    }
    if (transforming_ && t != Tool::Transform) {  // leaving Free Transform commits the live result
        commitTransform();
    }
    // Crosshair cursor for any other interactive tool
    // (Brush/Eraser/Marquee/Lasso/Crop/Bucket/Gradient/Eyedropper).
    setCursor(t == Tool::Hand        ? Qt::OpenHandCursor
              : t == Tool::Zoom      ? Qt::PointingHandCursor
              : t == Tool::Move      ? Qt::SizeAllCursor
              : t == Tool::Wand      ? Qt::PointingHandCursor
              : t == Tool::Type      ? Qt::IBeamCursor
              : t == Tool::Transform ? Qt::ArrowCursor
              : t == Tool::Inactive  ? Qt::ArrowCursor
                                     : Qt::CrossCursor);
    if (t == Tool::Transform) {
        setFocus();  // so Enter/Esc reach keyPressEvent
        // Begin a session only if one isn't already live, so re-selecting Free Transform (e.g.
        // pressing Ctrl+T again mid-edit) keeps the in-progress transform instead of dropping it.
        if (!transforming_) beginTransform();
    }
    if (changed) emit toolChanged(t);
}

std::size_t CanvasView::brushSlotFor(pe::PaintToolController::Mode m) noexcept {
    using Mode = pe::PaintToolController::Mode;
    // MaskPaint shares the Brush's settings: it IS the Brush, pointed at a mask.
    if (m == Mode::MaskPaint) m = Mode::Brush;
    const auto i = static_cast<std::size_t>(m);
    return i < kPaintModeCount ? i : 0;
}

void CanvasView::swapBrushSettingsTo(pe::PaintToolController::Mode m) {
    if (brushSlotFor(m) == brushSlotFor(brushSlotMode_)) {
        brushSlotMode_ = m;
        return;  // same slot (Brush and MaskPaint): nothing to carry across
    }
    brushPerMode_[brushSlotFor(brushSlotMode_)] = tool_.brush();
    tool_.brush() = brushPerMode_[brushSlotFor(m)];
    brushSlotMode_ = m;
    emit brushSettingsSwapped();
}

void CanvasView::setShowTransformControls(bool on) {
    if (showTransformControls_ == on) return;
    showTransformControls_ = on;
    update();  // the box appears or goes away immediately, not on the next unrelated repaint
}

void CanvasView::setMaskEditTarget(bool on) {
    if (maskEditTarget_ == on) return;
    // Drop any live stroke before flipping the target, mirroring setTool(): otherwise the next
    // mouse-move would rebuild the stroke under the new mode and commit the wrong thing.
    if (doc_ != nullptr && tool_.isStroking()) {
        tool_.cancel(*doc_);
        reloadImage();  // the cancel reverted tiles without notifying the renderer
    }
    maskEditTarget_ = on;
    // Only the Brush honors mask-edit; re-point its controller mode now (other tools are unaffected
    // and keep painting pixels). A no-op when the active tool isn't the Brush.
    if (toolMode_ == Tool::Brush) {
        tool_.setMode(on ? pe::PaintToolController::Mode::MaskPaint
                         : pe::PaintToolController::Mode::Brush);
    }
}

// ---------------------------------------------------------------- Free Transform

pe::PointD CanvasView::transformCenterDoc() const {
    return pe::PointD{transformBox_.x + transformBox_.width / 2.0 + tfTranslate_.x,
                      transformBox_.y + transformBox_.height / 2.0 + tfTranslate_.y};
}

pe::Affine2D CanvasView::transformMatrix() const {
    // Map the ORIGINAL box to its current placement: translate out to its center, scale + rotate
    // about that center, then apply the extra translation. (concat(a, b) applies b then a.)
    const double cx = transformBox_.x + transformBox_.width / 2.0;
    const double cy = transformBox_.y + transformBox_.height / 2.0;
    using A = pe::Affine2D;
    return A::concat(A::translation(cx + tfTranslate_.x, cy + tfTranslate_.y),
                     A::concat(A::rotation(tfAngle_), A::concat(A::scaling(tfScale_, tfScale_),
                                                                A::translation(-cx, -cy))));
}

void CanvasView::beginTransform() {
    cancelTransform();  // drop any prior session/preview first
    if (doc_ == nullptr) return;
    const pe::Layer* l = doc_->findLayer(doc_->activeLayer());
    // Only a pixel layer with content can be transformed; tell the user why nothing appeared.
    const pe::Rect b =
        (l != nullptr && l->kind() == pe::LayerKind::Pixel) ? l->contentBounds() : pe::Rect{};
    if (b.isEmpty()) {
        emit toolMessage(QStringLiteral("Free Transform needs a pixel layer with content."));
        return;
    }
    transformLayer_ = doc_->activeLayer();
    transformBox_ = b;
    tfScale_ = 1.0;
    tfAngle_ = 0.0;
    tfTranslate_ = pe::PointD{0.0, 0.0};
    tfDrag_ = -1;
    transforming_ = true;
    update();
}

void CanvasView::updateTransformPreview() {
    if (!transforming_ || doc_ == nullptr) return;
    pe::Rect reverted{};
    if (transformPreview_) {  // revert the prior preview so we always resample the ORIGINAL content
        reverted = transformPreview_->undo(*doc_).dirtyRegion;
        transformPreview_.reset();
    }
    // Bounded to what is on screen. The preview is rebuilt on EVERY motion event, and
    // resampling the whole layer each time cost about a second per mouse-move on a 16 MP
    // document, which is not a slow tool but an unusable one. Off-screen pixels keep their
    // original content until commitTransform builds the real, whole-layer command on
    // release, and the user cannot pan or zoom mid-drag to see the difference.
    transformPreview_ =
        pe::transformLayerContent(*doc_, transformLayer_, transformMatrix(), visibleDocRect());
    pe::Rect applied{};
    if (transformPreview_) applied = transformPreview_->execute(*doc_).dirtyRegion;
    // As in the Move drag: the revert's rect matters most when the rebuild returns null, and
    // the two are invalidated separately rather than united.
    repaintRegion(reverted);
    repaintRegion(applied);
    update();  // redraw the box/handles at their new placement
}

void CanvasView::commitTransform() {
    if (!transforming_) return;
    pe::Rect reverted{};
    if (transformPreview_ && doc_ != nullptr) {
        reverted = transformPreview_->undo(*doc_).dirtyRegion;  // back to S0
    }
    transformPreview_.reset();
    std::unique_ptr<pe::PaintCommand> cmd;
    if (doc_ != nullptr) cmd = pe::transformLayerContent(*doc_, transformLayer_, transformMatrix());
    transforming_ = false;
    tfDrag_ = -1;
    transformLayer_ = pe::kNoLayer;
    // The revert has to be invalidated either way: push notifies for the pixels the COMMAND
    // touches, which need not cover everything the reverted preview did.
    repaintRegion(reverted);
    if (cmd != nullptr && doc_ != nullptr) {
        doc_->history().push(std::move(cmd));  // one undo step; the observer refreshes the canvas
    }
    update();
}

void CanvasView::cancelTransform() {
    const bool was = transforming_;
    pe::Rect reverted{};
    if (transformPreview_ && doc_ != nullptr) {
        reverted = transformPreview_->undo(*doc_).dirtyRegion;  // restore the layer
    }
    transformPreview_.reset();
    transforming_ = false;
    tfDrag_ = -1;
    transformLayer_ = pe::kNoLayer;
    if (was) {
        repaintRegion(reverted);
        update();
    }
}

pe::Rect CanvasView::visibleDocRect() const {
    // The widget rect mapped back to document space, padded so a bilinear tap at the very
    // edge still reads the neighbour it needs and a rounding wobble cannot expose a seam.
    // Deliberately NOT clipped to the canvas: content dragged onto the pasteboard is still
    // on screen, and a preview that stopped at the canvas edge would tear there.
    constexpr int kPad = 4;
    const pe::PointD tl = view_.viewToDoc(pe::PointD{0.0, 0.0});
    const pe::PointD br =
        view_.viewToDoc(pe::PointD{static_cast<double>(width()), static_cast<double>(height())});
    const int l = static_cast<int>(std::floor(std::min(tl.x, br.x))) - kPad;
    const int t = static_cast<int>(std::floor(std::min(tl.y, br.y))) - kPad;
    const int r = static_cast<int>(std::ceil(std::max(tl.x, br.x))) + kPad;
    const int b = static_cast<int>(std::ceil(std::max(tl.y, br.y))) + kPad;
    if (r <= l || b <= t) return pe::Rect{};
    return pe::Rect{l, t, r - l, b - t};
}

QPointF CanvasView::docToWidget(pe::PointD docPos) const {
    const pe::PointD v = view_.docToView(docPos);
    return QPointF(v.x, v.y);
}

bool CanvasView::controlsGeometry(pe::Rect& box, pe::Affine2D& m) const {
    if (transforming_) {
        box = transformBox_;
        m = transformMatrix();
        return true;
    }
    // Show Transform Controls: the same handles on the active layer with nothing applied
    // yet, so grabbing one can start a real transform from exactly where the box sits.
    if (toolMode_ != Tool::Move || !showTransformControls_ || doc_ == nullptr) return false;
    const pe::Layer* l = doc_->findLayer(doc_->activeLayer());
    if (l == nullptr || l->kind() != pe::LayerKind::Pixel) return false;
    box = l->contentBounds();
    if (box.isEmpty()) return false;  // nothing to draw a box around, so no controls
    m = pe::Affine2D{};               // identity
    return true;
}

CanvasView::ControlPoints CanvasView::controlPointsFor(pe::Rect box, const pe::Affine2D& m) const {
    const double l = box.x;
    const double t = box.y;
    const double r = box.x + box.width;
    const double b = box.y + box.height;
    const double cxd[4] = {l, r, r, l};  // TL, TR, BR, BL
    const double cyd[4] = {t, t, b, b};
    ControlPoints cp;
    for (int i = 0; i < 4; ++i) {
        const pe::PointD v =
            view_.docToView(pe::PointD{m.applyX(cxd[i], cyd[i]), m.applyY(cxd[i], cyd[i])});
        cp.corner[i] = QPointF(v.x, v.y);
    }
    // The knob sits out past the top-edge midpoint, away from the centre. The centre is the
    // box centre through the same matrix, which is what transformCenterDoc() works out for a
    // live session and is also correct for the identity.
    const double mx = box.x + box.width / 2.0;
    const double my = box.y + box.height / 2.0;
    const pe::PointD cv = view_.docToView(pe::PointD{m.applyX(mx, my), m.applyY(mx, my)});
    const QPointF topMid((cp.corner[0].x() + cp.corner[1].x()) / 2.0,
                         (cp.corner[0].y() + cp.corner[1].y()) / 2.0);
    QPointF dir = topMid - QPointF(cv.x, cv.y);
    const double len = std::hypot(dir.x(), dir.y());
    dir = len > 1e-6 ? dir / len : QPointF(0.0, -1.0);
    cp.rotate = topMid + dir * kRotateHandleOffsetPx;
    return cp;
}

int CanvasView::hitTransformHandle(QPointF w) const {
    pe::Rect box{};
    pe::Affine2D m;
    if (!controlsGeometry(box, m)) return -1;
    const ControlPoints cp = controlPointsFor(box, m);
    const QPointF* cw = cp.corner;

    constexpr double kHandlePx = 9.0;
    for (int i = 0; i < 4; ++i) {
        if (std::hypot(w.x() - cw[i].x(), w.y() - cw[i].y()) <= kHandlePx)
            return i;  // corner=scale
    }
    if (std::hypot(w.x() - cp.rotate.x(), w.y() - cp.rotate.y()) <= kHandlePx) return 4;  // rotate
    // Inside the (convex) quad => move. Consistent-sign cross products over the 4 edges.
    const auto cross = [](QPointF a, QPointF c, QPointF p) {
        return (c.x() - a.x()) * (p.y() - a.y()) - (c.y() - a.y()) * (p.x() - a.x());
    };
    const double s0 = cross(cw[0], cw[1], w);
    const double s1 = cross(cw[1], cw[2], w);
    const double s2 = cross(cw[2], cw[3], w);
    const double s3 = cross(cw[3], cw[0], w);
    const bool allNonNeg = s0 >= 0 && s1 >= 0 && s2 >= 0 && s3 >= 0;
    const bool allNonPos = s0 <= 0 && s1 <= 0 && s2 <= 0 && s3 <= 0;
    return (allNonNeg || allNonPos) ? 5 : -1;  // 5 = inside (move)
}

void CanvasView::keyPressEvent(QKeyEvent* e) {
    if (transforming_) {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            commitTransform();
            return;
        }
        if (e->key() == Qt::Key_Escape) {
            cancelTransform();
            return;
        }
    }
    QWidget::keyPressEvent(e);
}

void CanvasView::maybeInitialFit() {
    if (needsFit_ && !canvasSize().isEmpty() && width() > 0 && height() > 0) {
        fitToWindow();
        needsFit_ = false;
    }
}

pe::StrokePoint CanvasView::sampleAt(QPointF widgetPos) const {
    // Map the device-space pointer position back to document space through the view
    // transform, so painting tracks the cursor under any zoom/pan.
    const pe::PointD d = view_.viewToDoc(pe::PointD{widgetPos.x(), widgetPos.y()});
    // Mouse input carries no pressure; tablet pressure/tilt arrive in a later pass.
    return pe::StrokePoint{{static_cast<float>(d.x), static_cast<float>(d.y)}, 1.0f};
}

TaskResult CanvasView::runCanvasTask(const QString& title, TaskAccess access,
                                     const std::function<void()>& work) {
    if (taskRunner_) return taskRunner_(title, access, work);
    return runDocumentTask(this, this, title, access, work);
}

void CanvasView::setFrozen(bool on) {
    if (frozen_ == on) return;
    if (on) {
        // Grabbed BEFORE the flag is set, so this last render still goes through the
        // renderer and captures the live document rather than an empty pixmap.
        frozenFrame_ = grab();
    } else {
        frozenFrame_ = QPixmap{};
    }
    frozen_ = on;
    update();
}

void CanvasView::paintEvent(QPaintEvent*) {
    if (frozen_) {
        // A worker owns the document; compositing here would race it. Everything below
        // this point reads the document or the renderer, so none of it may run.
        QPainter frozenPainter(this);
        frozenPainter.fillRect(rect(), themeColors(currentTheme()).canvas);
        if (!frozenFrame_.isNull()) frozenPainter.drawPixmap(0, 0, frozenFrame_);
        return;
    }

    maybeInitialFit();

    QPainter painter(this);
    painter.fillRect(rect(), themeColors(currentTheme()).canvas);  // themed pasteboard
    const pe::Size cs = canvasSize();
    if (cs.isEmpty() || renderer_ == nullptr) return;

    // Device-space rectangle the whole canvas maps to (rotation is not exposed yet, so
    // doc->view is scale + translate and the canvas stays axis-aligned).
    const pe::PointD tl = view_.docToView(pe::PointD{0.0, 0.0});
    const pe::PointD br =
        view_.docToView(pe::PointD{static_cast<double>(cs.width), static_cast<double>(cs.height)});
    const QRectF devRect(QPointF(tl.x, tl.y), QPointF(br.x, br.y));
    painter.fillRect(devRect, checker_);  // transparency shows through as a checkerboard

    // Crisp pixels when magnifying, smooth when minifying.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, view_.zoom() < 1.0);
    painter.setTransform(toQTransform(view_.docToView()));

    // Composite ONLY the visible canvas region: a huge canvas never composites the whole image,
    // and panning recomposites just the newly-exposed tiles. Within the composite budget this goes
    // through the LRU tile cache; beyond it (extreme zoom-out, a >64 MP visible region) we
    // composite a bounded downscale sized to ~the viewport and stretch it to fill — so the canvas
    // no longer blanks when fully zoomed out.
    const pe::Rect canvas{0, 0, cs.width, cs.height};
    const pe::Rect vis = view_.visibleDocRect(pe::Size{width(), height()}).intersected(canvas);
    if (!vis.isEmpty()) {
        // Branch on the VIEWPORT, not the engine's 64 MP composite cap. The old test meant a
        // repaint at 12% zoom on an 8000 square document allocated and filled a 64 MP buffer
        // to draw 1.6 MP of screen: 195 ms, and 2.8 seconds cold, while zooming out FURTHER
        // got cheaper. The curve was inverted.
        //
        // Even with every tile cached, the full-resolution path still assembles an output
        // buffer the size of the visible region, so its cost is O(visible area) whatever the
        // cache does. That is why the test is the ratio and not whether the tiles fit.
        const std::int64_t viewportPx =
            std::max<std::int64_t>(1, static_cast<std::int64_t>(width()) * height());
        const std::int64_t visArea =
            static_cast<std::int64_t>(vis.width) * static_cast<std::int64_t>(vis.height);
        if (visArea <= viewportPx) {
            // Fits the cache, so the full-resolution path is both exact and incremental: it
            // recomposites only the tiles that actually changed, which is what keeps
            // painting cheap. This is the interactive case and it must stay on this path.
            pe::PixelBuffer buf = renderer_->renderRegion(vis);  // alive through drawImage
            // Ours to modify: renderRegion returns a freshly assembled buffer, not a view
            // into the cache. A no-op unless a channel view is on.
            pe::applyChannelView(buf, channelView_);
            if (!buf.isEmpty()) {
                const QImage img(reinterpret_cast<const uchar*>(buf.data()), buf.width(),
                                 buf.height(), buf.width() * 4, QImage::Format_RGBA8888);
                painter.drawImage(QPointF(vis.x, vis.y), img);
            }
        } else {
            // More visible tiles than the cache can hold. The full-res path would recomposite
            // all of them every frame anyway (they evict each other) AND allocate a buffer far
            // larger than the screen, so composite to about the viewport's pixel count instead
            // and stretch. Cost is then bounded by the window rather than the document.
            //
            // Cached, so a repaint that changes neither the region nor the document is free:
            // a resize, or another window uncovering the canvas, no longer recomposites.
            // Four times the viewport, not one: the scale factor is a power of two, so a
            // cap of exactly one viewport would round s UP to the next power and composite
            // as much as twice coarser than the screen shows, which looks soft. At 4x, s
            // lands strictly below 1/zoom, so the composite is always at least as fine as
            // the display. The buffer stays bounded (about 25 MB at a 1600x1000 viewport).
            const int cap = static_cast<int>(std::min<std::int64_t>(viewportPx * 4, 1 << 26));
            pe::Rect covered{};
            const pe::PixelBuffer& cached = renderer_->renderRegionScaledCached(vis, cap, covered);
            // Unlike the branch above, this is a reference INTO the renderer's cache, so a
            // channel view has to work on a copy: applying it in place would poison the cache
            // and the next repaint would draw a channel view of a channel view. The copy is
            // paid for only while a channel view is on, and only on this zoomed-way-out path.
            pe::PixelBuffer viewed;
            if (!channelView_.showsAll() && !cached.isEmpty()) {
                viewed = cached;
                pe::applyChannelView(viewed, channelView_);
            }
            const pe::PixelBuffer& buf = viewed.isEmpty() ? cached : viewed;
            if (!buf.isEmpty()) {
                const QImage img(reinterpret_cast<const uchar*>(buf.data()), buf.width(),
                                 buf.height(), buf.width() * 4, QImage::Format_RGBA8888);
                // Drawn to the TILE-ALIGNED rect the buffer actually covers, which is at
                // least `vis`; the overhang lies off-screen or over the pasteboard, where it
                // composites to transparent.
                painter.drawImage(QRectF(covered.x, covered.y, covered.width, covered.height), img,
                                  QRectF(img.rect()));
            }
        }
    }

    // --- basic marching ants + live marquee overlay (device space) ---
    // For real marching, a timer would offset the dash; here we draw a visible
    // dashed outline of the active selection bounds (and any in-progress drag).
    if (doc_ != nullptr) {
        // Cosmetic pens stay 1 device pixel wide (and keep a fixed dash length) regardless
        // of zoom — without this the dashes are drawn in document units through the
        // docToView transform and visibly thicken as you zoom in.
        QPen antPen(QColor(0, 0, 0), 1, Qt::DashLine);
        antPen.setCosmetic(true);
        antPen.setDashOffset(0);
        QPen antPen2(QColor(255, 255, 255), 1, Qt::DashLine);
        antPen2.setCosmetic(true);
        antPen2.setDashOffset(2);  // offset gives the classic alternating look
        const QPen* pens[2] = {&antPen, &antPen2};

        auto drawAntRect = [&](Rect r) {
            if (r.width <= 0 || r.height <= 0) return;
            // Draw in document coordinates; the active docToView transform maps it.
            const QRectF dr(r.x, r.y, r.width, r.height);
            for (const QPen* pen : pens) {
                painter.setPen(*pen);
                painter.drawRect(dr);
            }
        };

        // In-progress freehand lasso path, then live marquee rect, then the committed
        // selection's pixel-tight outline (cached on the last selection change, so the
        // per-pixel scan does not run every repaint).
        if (draggingLasso_ && lassoPts_.size() >= 2) {
            std::vector<QPointF> poly;
            poly.reserve(lassoPts_.size() + 1);
            for (const pe::Point& p : lassoPts_) poly.emplace_back(p.x, p.y);
            poly.emplace_back(lassoPts_.front().x, lassoPts_.front().y);  // close the loop
            for (const QPen* pen : pens) {
                painter.setPen(*pen);
                painter.drawPolyline(poly.data(), static_cast<int>(poly.size()));
            }
        } else if (draggingMarquee_ && liveMarquee_.width > 0 && liveMarquee_.height > 0) {
            drawAntRect(liveMarquee_);
        } else if (!selectionAnts_.isEmpty()) {
            // The real boundary, as one call per pen rather than one per segment.
            for (const QPen* pen : pens) {
                painter.setPen(*pen);
                painter.drawLines(selectionAnts_);
            }
        } else {
            // Only reached for a selection whose outline could not be traced; empty
            // otherwise, and drawAntRect ignores an empty rect.
            drawAntRect(selectionAntsBounds_);
        }
    }

    // Gradient tool: while dragging, draw a guide line from the drag start to the cursor. It is
    // a UI overlay in widget (device-independent) space, so reset the doc->view transform first
    // and use cosmetic pens (a black line under a white dashed one, like the marching ants).
    if (draggingGradient_ && doc_ != nullptr) {
        painter.resetTransform();
        QPen guideBlack(QColor(0, 0, 0), 1);
        guideBlack.setCosmetic(true);
        painter.setPen(guideBlack);
        painter.drawLine(gradStartWidget_, gradEndWidget_);
        QPen guideWhite(QColor(255, 255, 255), 1, Qt::DashLine);
        guideWhite.setCosmetic(true);
        painter.setPen(guideWhite);
        painter.drawLine(gradStartWidget_, gradEndWidget_);
    }

    // The transform box: dashed black-under-white like the marching ants, four corner scale
    // handles, and a rotate knob above the top edge, all in widget space. Shown for a live
    // Free Transform session, and for the Move tool when Show Transform Controls is on.
    pe::Rect ctlBox{};
    pe::Affine2D ctlMatrix;
    if (controlsGeometry(ctlBox, ctlMatrix)) {
        painter.resetTransform();
        const ControlPoints cp = controlPointsFor(ctlBox, ctlMatrix);
        const QPointF* cw = cp.corner;
        const QPointF poly[5] = {cw[0], cw[1], cw[2], cw[3], cw[0]};
        QPen edgeBlack(QColor(0, 0, 0), 1);
        edgeBlack.setCosmetic(true);
        QPen edgeWhite(QColor(255, 255, 255), 1, Qt::DashLine);
        edgeWhite.setCosmetic(true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(edgeBlack);
        painter.drawPolyline(poly, 5);
        painter.setPen(edgeWhite);
        painter.drawPolyline(poly, 5);

        const QPointF topMid((cw[0].x() + cw[1].x()) / 2.0, (cw[0].y() + cw[1].y()) / 2.0);
        painter.setPen(edgeBlack);
        painter.drawLine(topMid, cp.rotate);

        painter.setPen(QPen(QColor(0, 0, 0), 1));
        painter.setBrush(QColor(255, 255, 255));
        constexpr double hs = 4.0;  // handle half-size (widget px)
        for (int i = 0; i < 4; ++i) {
            painter.drawRect(QRectF(cw[i].x() - hs, cw[i].y() - hs, 2 * hs, 2 * hs));
        }
        painter.drawEllipse(cp.rotate, hs, hs);
    }
}

void CanvasView::tabletEvent(QTabletEvent* e) {
    if (doc_ == nullptr) {
        e->ignore();
        return;
    }
    if (toolMode_ != Tool::Brush && toolMode_ != Tool::Eraser && toolMode_ != Tool::Dodge &&
        toolMode_ != Tool::Clone && toolMode_ != Tool::Blur && toolMode_ != Tool::Heal) {
        e->ignore();
        return;
    }
    pe::StrokePoint sp = sampleAt(e->position());
    sp.pressure = std::clamp(static_cast<float>(e->pressure()), 0.0f, 1.0f);
    switch (e->type()) {
        case QEvent::TabletPress:
            if (tool_.isStroking()) tool_.cancel(*doc_);
            if (toolMode_ == Tool::Clone &&
                handleClonePress(view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()}),
                                 (e->modifiers() & Qt::AltModifier) != 0)) {
                e->accept();  // Alt set the source / no source yet — no stroke begins
                return;
            }
            if (toolMode_ == Tool::Dodge) {
                tool_.setMode((e->modifiers() & Qt::AltModifier)
                                  ? pe::PaintToolController::Mode::Burn
                                  : pe::PaintToolController::Mode::Dodge);
            }
            if (toolMode_ == Tool::Blur) {
                tool_.setMode((e->modifiers() & Qt::AltModifier)
                                  ? pe::PaintToolController::Mode::Sharpen
                                  : pe::PaintToolController::Mode::Blur);
            }
            if (tool_.begin(*doc_, sp, &doc_->selection())) {
                if (renderer_ != nullptr) renderer_->invalidate(tool_.lastExtendBounds());
                update();
            } else {
                emit toolMessage(paintUnavailableMessage());
            }
            break;
        case QEvent::TabletMove:
            if (tool_.isStroking()) {
                tool_.extend(*doc_, sp);
                if (renderer_ != nullptr) renderer_->invalidate(tool_.lastExtendBounds());
                update();
            }
            break;
        case QEvent::TabletRelease:
            if (tool_.isStroking()) {
                const pe::Rect dirty = tool_.strokeDirtyBounds();  // capture before end() clears it
                const bool atBudget = tool_.strokeAtBudget();
                tool_.end(*doc_);
                if (renderer_ != nullptr) renderer_->invalidate(dirty);  // covers no-deposit too
                if (atBudget) emit toolMessage(strokeAtBudgetMessage());
                update();
            }
            break;
        default:
            break;
    }
    e->accept();
}

void CanvasView::mousePressEvent(QMouseEvent* e) {
    // Pan on middle-drag, or on left-drag while the Hand tool is active.
    const bool wantPan = e->button() == Qt::MiddleButton ||
                         (e->button() == Qt::LeftButton && toolMode_ == Tool::Hand);
    if (wantPan) {
        // Abandon an in-progress gradient drag: panning shifts the view transform, so the
        // captured widget endpoints would map to the wrong document points on release.
        if (draggingGradient_) draggingGradient_ = false;
        panning_ = true;
        lastPanPos_ = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || doc_ == nullptr) {
        QWidget::mousePressEvent(e);
        return;
    }
    if (toolMode_ == Tool::Transform) {
        if (!transforming_) beginTransform();  // (re)start a session if none is live
        if (!transforming_) return;            // no active pixel layer with content
        const int hit = hitTransformHandle(e->position());
        if (hit < 0) {  // a click outside the box commits the transform (Photoshop behavior)
            commitTransform();
            return;
        }
        tfDrag_ = hit;
        tfDragStartDoc_ = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        tfDragStartScale_ = tfScale_;
        tfDragStartAngle_ = tfAngle_;
        tfDragStartTranslate_ = tfTranslate_;
        return;
    }
    if (toolMode_ == Tool::Zoom) {
        // Click zooms in about the cursor; Alt-click zooms out.
        const double factor = (e->modifiers() & Qt::AltModifier) ? 1.0 / kZoomStep : kZoomStep;
        const double target = std::clamp(view_.zoom() * factor, pe::kMinZoom, pe::kMaxZoom);
        const QPointF p = e->position();
        view_.zoomAround(pe::PointD{p.x(), p.y()}, target);
        needsFit_ = false;
        update();
        emit zoomChanged(zoomPercent());
        return;
    }
    if (toolMode_ == Tool::Marquee || toolMode_ == Tool::Crop) {
        draggingMarquee_ = true;  // shared rubber-band drag; commit differs by tool (see release)
        marqueeAnchor_ = e->position();
        liveMarquee_ = Rect{};
        update();
        return;
    }
    if (toolMode_ == Tool::Eyedropper && renderer_ != nullptr && !canvasSize().isEmpty()) {
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const int ix = std::clamp(static_cast<int>(std::lround(d.x)), 0, canvasSize().width - 1);
        const int iy = std::clamp(static_cast<int>(std::lround(d.y)), 0, canvasSize().height - 1);
        const pe::PixelBuffer px =
            renderer_->renderRegion(pe::Rect{ix, iy, 1, 1});  // composite 1px
        if (!px.isEmpty()) {
            const pe::Rgba8 c = px.at(0, 0);
            emit colorPicked(QColor(c.r, c.g, c.b, c.a));
        }
        return;
    }
    if (toolMode_ == Tool::Move) {
        moveRefusalSaid_ = false;  // a new drag gets to explain itself again
        // Recover from any stale (capture-lost) preview. This used to revert without
        // repainting at all, leaving the reverted pixels stale on screen until something
        // else happened to invalidate them; having the rect closes that too.
        repaintRegion(cancelMovePreview());

        // A corner or the rotate knob starts a transform, which is what drawing the box is
        // for. Inside the box (5) or outside it (-1) is still a move, so both fall through.
        // hitTransformHandle answers -1 unless the controls are actually shown.
        const int handle = hitTransformHandle(e->position());
        if (handle >= 0 && handle != 5) {
            setTool(Tool::Transform);    // begins the session, takes focus, tells the shell
            if (!transforming_) return;  // no transformable layer: setTool already said why
            tfDrag_ = handle;
            tfDragStartDoc_ = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
            tfDragStartScale_ = tfScale_;
            tfDragStartAngle_ = tfAngle_;
            tfDragStartTranslate_ = tfTranslate_;
            return;
        }

        if (autoSelect_) {
            const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
            const pe::Point p{static_cast<int>(std::lround(d.x)),
                              static_cast<int>(std::lround(d.y))};
            pe::LayerId picked = pe::layerAt(*doc_, p);
            if (picked != pe::kNoLayer && autoSelectMode_ == AutoSelectMode::Group) {
                picked = pe::topLevelAncestorOf(*doc_, picked);
            }
            if (picked == pe::kNoLayer) {
                // Nothing visible under the cursor. Dragging the active layer instead is
                // exactly the surprise Auto-Select exists to remove, so no drag starts.
                emit toolMessage(QStringLiteral("Auto-Select: nothing under the cursor."));
                return;
            }
            doc_->setActiveLayer(picked);  // notifies, so the Layers panel follows the click
        }

        movingContent_ = true;
        moveStartWidget_ = e->position();
        moveLayer_ = doc_->activeLayer();  // move the layer that is active when the drag begins
        return;
    }
    if (toolMode_ == Tool::Lasso) {
        draggingLasso_ = true;
        lassoPts_.clear();
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        lassoPts_.push_back(
            pe::Point{static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y))});
        update();
        return;
    }
    if (toolMode_ == Tool::Wand) {
        if (renderer_ == nullptr) return;
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const pe::Point seed{static_cast<int>(std::lround(d.x)),
                             static_cast<int>(std::lround(d.y))};

        // Sampling and flood-filling a whole canvas took 2.3 seconds on a 24 MP document,
        // and it ran here, on the GUI thread, on a single click: long enough that the tool
        // was indistinguishable from a hang. Both halves now run on a worker.
        //
        // Through the renderer's tile cache rather than Document::compositeImage(): the
        // pixels are identical, but the paint path has already composited most of these
        // tiles, so a click reuses them instead of flattening the whole canvas again.
        pe::Selection sel;
        bool overBudget = false;
        // LiveDocument, not Snapshot: the wand deliberately samples the renderer's tile
        // cache, which the paint path has already warmed. A snapshot would arrive with a
        // cold renderer and pay back the full composite this change removed.
        const TaskResult task = runCanvasTask(
            QStringLiteral("Magic Wand"), TaskAccess::LiveDocument,
            [this, seed, &sel, &overBudget] {
                const pe::PixelBuffer buf = renderer_->renderRegion(doc_->canvasBounds());
                if (buf.isEmpty()) {
                    overBudget = true;
                    return;
                }
                sel = pe::magicWandSelection(buf, seed.x, seed.y, wandTolerance_);
            });
        if (task.threw) {
            emit toolMessage(QStringLiteral("Magic Wand failed: %1").arg(task.error));
            return;
        }
        if (!task.ran) return;
        if (overBudget) {
            // The composite budget applies whether the pixels come from the renderer or
            // from Document::compositeImage(), so on a canvas past that cap the wand used
            // to select nothing and say nothing.
            emit toolMessage(
                QStringLiteral("Magic Wand needs to flatten the image, and this one is over "
                               "the %1 megapixel limit.")
                    .arg(pe::kMaxCompositeImagePixels / 1'000'000));
            return;
        }
        if (sel.active()) {
            doc_->history().push(std::make_unique<SetSelectionCommand>(std::move(sel)));
        } else if (doc_->canvasBounds().contains(seed)) {
            emit toolMessage(QStringLiteral("Magic Wand selected nothing at that point."));
        }
        return;
    }
    if (toolMode_ == Tool::Bucket) {
        // Flood-fill from the clicked pixel with the foreground color, gated by the selection.
        // bucketFill returns a command (nothing applied yet); push() executes + notifies so the
        // renderer-observer marks the touched tiles dirty and the canvas repaints.
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const pe::Point seed{static_cast<int>(std::lround(d.x)),
                             static_cast<int>(std::lround(d.y))};
        constexpr int kBucketTolerance = 32;  // default per-channel tolerance (v1)
        if (auto cmd = pe::bucketFill(*doc_, doc_->activeLayer(), seed.x, seed.y, tool_.color(),
                                      kBucketTolerance, &doc_->selection())) {
            doc_->history().push(std::move(cmd));
        } else if (doc_->canvasBounds().contains(seed)) {
            // An on-canvas click that filled nothing is a failure worth reporting; an off-canvas
            // click (on the pasteboard) is simply ignored.
            emit toolMessage(fillUnavailableMessage());
        }
        return;
    }
    if (toolMode_ == Tool::Gradient) {
        // Begin a gradient drag: a guide line is drawn until release, when the foreground->
        // background gradient is applied along start->end (see mouseReleaseEvent).
        draggingGradient_ = true;
        gradStartWidget_ = e->position();
        gradEndWidget_ = e->position();
        update();
        return;
    }
    if (toolMode_ == Tool::Type) {
        // Report an on-canvas click in document space; MainWindow prompts for the text and stamps
        // it (rasterized) onto the active layer as an undoable command. Off-canvas (pasteboard)
        // clicks are ignored, like the other click tools, so text can't land outside the image.
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const pe::Point p{static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y))};
        if (doc_->canvasBounds().contains(p)) {
            emit textRequested(QPointF(d.x, d.y));
        } else {
            // A click on the pasteboard used to be ignored in silence, which reads as the
            // Type tool being broken rather than as the click being off the image.
            emit refused(pe::refuse("tool.type.place", pe::RefusalCode::PointOutsideCanvas,
                                    "Type tool click", "Click inside the image to place text.",
                                    "click at (" + std::to_string(p.x) + ", " +
                                        std::to_string(p.y) + "); canvas is " +
                                        std::to_string(doc_->canvasSize().width) + "x" +
                                        std::to_string(doc_->canvasSize().height)));
        }
        return;
    }
    if (toolMode_ == Tool::Clone) {
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        if (handleClonePress(d, (e->modifiers() & Qt::AltModifier) != 0)) return;
        // else: fall through to begin a clone stroke (mode is Clone via setTool).
    }
    if (toolMode_ != Tool::Brush && toolMode_ != Tool::Eraser && toolMode_ != Tool::Dodge &&
        toolMode_ != Tool::Clone && toolMode_ != Tool::Blur && toolMode_ != Tool::Heal) {
        return;  // Inactive: no paint
    }
    if (toolMode_ == Tool::Dodge) {
        // Alt temporarily switches the tone brush from Dodge (lighten) to Burn (darken) for this
        // stroke, the Photoshop convention — set per stroke since the modifier is read at press.
        tool_.setMode((e->modifiers() & Qt::AltModifier) ? pe::PaintToolController::Mode::Burn
                                                         : pe::PaintToolController::Mode::Dodge);
    }
    if (toolMode_ == Tool::Blur) {
        // Alt temporarily switches the Blur tool to Sharpen for this stroke, mirroring the
        // Dodge/Burn convention — set per stroke since the modifier is read at press.
        tool_.setMode((e->modifiers() & Qt::AltModifier) ? pe::PaintToolController::Mode::Sharpen
                                                         : pe::PaintToolController::Mode::Blur);
    }
    // Recover if a prior stroke never received its release (e.g. mouse capture was
    // stolen by a modal/Alt-Tab): drop the stale preview before starting fresh.
    if (tool_.isStroking()) tool_.cancel(*doc_);
    // Begin a stroke; the first dab is a live preview the observer won't see, so
    // refresh explicitly. begin() fails when there is no paintable active layer.
    // Pass the document selection so edits are gated.
    if (tool_.begin(*doc_, sampleAt(e->position()), &doc_->selection())) {
        if (renderer_ != nullptr) renderer_->invalidate(tool_.lastExtendBounds());  // first dab
        update();
    } else {
        // Bucket and Gradient already report this case; the brush path silently did
        // nothing, which is indistinguishable from the tool being broken.
        emit toolMessage(paintUnavailableMessage());
    }
}

void CanvasView::leaveEvent(QEvent* e) {
    emit cursorLeft();
    QWidget::leaveEvent(e);
}

void CanvasView::mouseMoveEvent(QMouseEvent* e) {
    // Before the drag dispatch below, so the readout keeps up during a stroke too.
    if (doc_ != nullptr) {
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        emit cursorMoved(QPointF(d.x, d.y));
    }
    if (panning_) {
        const QPointF p = e->position();
        view_.panByView(p.x() - lastPanPos_.x(), p.y() - lastPanPos_.y());
        lastPanPos_ = p;
        update();
        return;
    }
    if (draggingMarquee_ && doc_ != nullptr) {
        // Live update the marquee rect in doc space
        const pe::PointD a = view_.viewToDoc(pe::PointD{marqueeAnchor_.x(), marqueeAnchor_.y()});
        const pe::PointD b = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const int x0 = static_cast<int>(std::floor(std::min(a.x, b.x)));
        const int y0 = static_cast<int>(std::floor(std::min(a.y, b.y)));
        const int x1 = static_cast<int>(std::ceil(std::max(a.x, b.x)));
        const int y1 = static_cast<int>(std::ceil(std::max(a.y, b.y)));
        liveMarquee_ = Rect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
        update();
        return;
    }
    if (draggingLasso_ && doc_ != nullptr) {
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const pe::Point p{static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y))};
        if (lassoPts_.empty() || !(lassoPts_.back() == p)) lassoPts_.push_back(p);  // dedup
        update();
        return;
    }
    if (draggingGradient_ && doc_ != nullptr) {
        gradEndWidget_ = e->position();
        update();  // redraw the guide line to the new cursor position
        return;
    }
    if (transforming_ && tfDrag_ >= 0 && doc_ != nullptr) {
        const pe::PointD now = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        if (tfDrag_ == 5) {  // move: translate by the doc-space drag delta
            tfTranslate_ = pe::PointD{tfDragStartTranslate_.x + (now.x - tfDragStartDoc_.x),
                                      tfDragStartTranslate_.y + (now.y - tfDragStartDoc_.y)};
        } else if (tfDrag_ == 4) {  // rotate about the box center by the angular delta
            const pe::PointD c = transformCenterDoc();
            const double a0 = std::atan2(tfDragStartDoc_.y - c.y, tfDragStartDoc_.x - c.x);
            const double a1 = std::atan2(now.y - c.y, now.x - c.x);
            tfAngle_ = tfDragStartAngle_ + (a1 - a0);
        } else {  // corner (0..3): uniform scale about the center by the radial-distance ratio
            const pe::PointD c = transformCenterDoc();
            const double r0 = std::hypot(tfDragStartDoc_.x - c.x, tfDragStartDoc_.y - c.y);
            const double r1 = std::hypot(now.x - c.x, now.y - c.y);
            if (r0 > 1e-6) {
                tfScale_ = std::clamp(tfDragStartScale_ * (r1 / r0), 0.01, 100.0);
            }
        }
        updateTransformPreview();
        return;
    }
    if (movingContent_ && doc_ != nullptr) {
        // Cumulative drag delta in document pixels, applied to the ORIGINAL content each move
        // (the prior preview is reverted first), so the result equals one move by the total.
        const pe::PointD a =
            view_.viewToDoc(pe::PointD{moveStartWidget_.x(), moveStartWidget_.y()});
        const pe::PointD b = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const int dx = static_cast<int>(std::lround(b.x - a.x));
        const int dy = static_cast<int>(std::lround(b.y - a.y));
        // Both rects are needed, and both must be invalidated even when the rebuild returns
        // null: the previous preview has already been reverted by then, so the revert's rect
        // is the only thing keeping the cache honest. Dragging back to the exact start is the
        // ordinary way to reach that, since a zero delta yields no command at all.
        pe::Rect reverted{};
        if (movePreview_) {
            reverted = movePreview_->undo(*doc_).dirtyRegion;
            movePreview_.reset();
        }
        movePreview_ = pe::moveLayerContent(*doc_, moveLayer_, dx, dy);
        pe::Rect applied{};
        if (movePreview_) {
            applied = movePreview_->execute(*doc_).dirtyRegion;
        } else if (!moveRefusalSaid_) {
            // A refused Move used to be completely silent, which is the whole of #180 and is
            // indistinguishable from a broken tool. Said ONCE per drag, not per motion event,
            // and not at all for the ordinary case of dragging back to the start, which is a
            // NoEffect and not something the user needs told.
            const pe::Refusal why = pe::moveRefusal(*doc_, moveLayer_, dx, dy);
            if (why.isRefusal() && why.code != pe::RefusalCode::NoEffect) {
                moveRefusalSaid_ = true;
                emit toolMessage(QString::fromStdString(why.explanation));
            }
        }
        // Two calls rather than one united rect: uniting a rect near the drag origin with one
        // far away yields a bounding box that can dwarf both, and that is the only realistic
        // way to trip invalidate()'s escalation back to dropping the whole cache.
        repaintRegion(reverted);
        repaintRegion(applied);
        return;
    }
    if (!tool_.isStroking() || doc_ == nullptr) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    tool_.extend(*doc_, sampleAt(e->position()));
    // Just this sample's footprint. Invalidating the cumulative bounds re-composited
    // every tile under the stroke on every sample, so repaint cost grew with the
    // stroke; end() below still invalidates the whole thing.
    if (renderer_ != nullptr) renderer_->invalidate(tool_.lastExtendBounds());
    update();
}

void CanvasView::mouseReleaseEvent(QMouseEvent* e) {
    if (panning_ && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        panning_ = false;
        setCursor(toolMode_ == Tool::Hand   ? Qt::OpenHandCursor
                  : toolMode_ == Tool::Zoom ? Qt::PointingHandCursor
                                            : Qt::CrossCursor);
        return;
    }
    // Free Transform: a left-release just ends the current handle drag — the transform session
    // stays live (more handle drags, then Enter / click-outside commits, Esc cancels).
    if (transforming_ && tfDrag_ >= 0 && e->button() == Qt::LeftButton) {
        tfDrag_ = -1;
        return;
    }
    // The drag tools (marquee/crop, lasso, gradient, move) commit only on a LEFT-button release,
    // like the brush path below. A non-left release mid-drag falls through to that final guard
    // (which returns), so the gesture is left intact and continues until the left button is up —
    // pressing a second button no longer prematurely commits the gesture.
    if (draggingMarquee_ && doc_ != nullptr && e->button() == Qt::LeftButton) {
        draggingMarquee_ = false;
        if (liveMarquee_.width > 0 && liveMarquee_.height > 0) {
            if (toolMode_ == Tool::Crop) {
                // Crop commits a CropCommand (resize + content shift) and re-fits the view to
                // the new, smaller canvas. The command clamps the rect to the canvas itself.
                doc_->history().push(std::make_unique<CropCommand>(liveMarquee_));
                fitToWindow();
            } else {
                Selection target = doc_->selection();
                Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
                if (mods & Qt::ShiftModifier) {
                    target.addRect(liveMarquee_);
                } else if (mods & (Qt::AltModifier | Qt::ControlModifier)) {
                    target.subtractRect(liveMarquee_);
                } else {
                    target.selectRect(liveMarquee_);
                }
                doc_->history().push(std::make_unique<SetSelectionCommand>(std::move(target)));
                // command will execute, snapshot old, notify
            }
        }
        liveMarquee_ = Rect{};
        update();
        return;
    }
    if (draggingLasso_ && doc_ != nullptr && e->button() == Qt::LeftButton) {
        draggingLasso_ = false;
        if (lassoPts_.size() >= 3) {
            Selection target;
            target.selectPolygon(lassoPts_);
            if (target.active()) {
                doc_->history().push(std::make_unique<SetSelectionCommand>(std::move(target)));
            }
        }
        lassoPts_.clear();
        update();
        return;
    }
    if (draggingGradient_ && e->button() == Qt::LeftButton) {
        draggingGradient_ = false;
        if (doc_ != nullptr) {
            // Map the guide endpoints back to document space and apply a foreground->background
            // linear gradient along start->end. gradientFill no-ops on a zero-length drag and
            // honors the active selection; push() executes + notifies (observer repaints).
            const pe::PointD a =
                view_.viewToDoc(pe::PointD{gradStartWidget_.x(), gradStartWidget_.y()});
            const pe::PointD b =
                view_.viewToDoc(pe::PointD{gradEndWidget_.x(), gradEndWidget_.y()});
            const pe::Point start{static_cast<int>(std::lround(a.x)),
                                  static_cast<int>(std::lround(a.y))};
            const pe::Point end{static_cast<int>(std::lround(b.x)),
                                static_cast<int>(std::lround(b.y))};
            const pe::Rgbaf fg = tool_.color();  // foreground = near stop (at start)
            const pe::Rgbaf bg{static_cast<float>(bgColor_.redF()),
                               static_cast<float>(bgColor_.greenF()),
                               static_cast<float>(bgColor_.blueF()),
                               static_cast<float>(bgColor_.alphaF())};  // background = far stop
            if (auto cmd = pe::gradientFill(*doc_, doc_->activeLayer(), start, end, fg, bg,
                                            &doc_->selection())) {
                doc_->history().push(std::move(cmd));
            } else if (start.x != end.x || start.y != end.y) {
                // A genuine drag (not a zero-length click) that produced nothing is a failure
                // worth reporting; a click without a drag is silently ignored.
                emit toolMessage(fillUnavailableMessage());
            }
        }
        update();
        return;
    }
    if (movingContent_ && e->button() == Qt::LeftButton) {
        movingContent_ = false;
        moveLayer_ = pe::kNoLayer;
        if (movePreview_ && doc_ != nullptr) {
            // No explicit invalidation here, unlike commitTransform. The command being pushed
            // IS the preview being reverted, and PaintCommand reports the same rect from undo
            // as from execute, so push's notification covers exactly what the revert dirtied.
            // commitTransform differs because it reverts the preview and pushes a DIFFERENT,
            // freshly built command, whose rect need not cover the preview's.
            (void)movePreview_->undo(*doc_);
            doc_->history().push(
                std::move(movePreview_));  // commit one undo step; observer refreshes
        } else {
            update();  // dragged back to origin: the last mouse-move already invalidated
        }
        return;
    }
    if (e->button() != Qt::LeftButton || !tool_.isStroking()) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    const pe::Rect dirty = tool_.strokeDirtyBounds();  // capture before end() clears it
    const bool atBudget = tool_.strokeAtBudget();      // end() resets it
    tool_.end(*doc_);  // commits one undoable command; the renderer-observer marks it dirty
    if (renderer_ != nullptr) renderer_->invalidate(dirty);  // also covers a no-deposit stroke
    // A stroke that stopped following the cursor needs to say why, or it reads as a freeze.
    if (atBudget) emit toolMessage(strokeAtBudgetMessage());
    update();
}

void CanvasView::wheelEvent(QWheelEvent* e) {
    if (doc_ == nullptr) {
        QWidget::wheelEvent(e);
        return;
    }
    const double notches = e->angleDelta().y() / 120.0;
    if (notches == 0.0) {
        QWidget::wheelEvent(e);
        return;
    }
    needsFit_ = false;
    const double target =
        std::clamp(view_.zoom() * std::pow(kZoomStep, notches), pe::kMinZoom, pe::kMaxZoom);
    const QPointF pos = e->position();
    view_.zoomAround(pe::PointD{pos.x(), pos.y()}, target);  // keep the cursor's pixel fixed
    update();
    emit zoomChanged(zoomPercent());
}

void CanvasView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    maybeInitialFit();
}

void CanvasView::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    maybeInitialFit();
}

}  // namespace pe::app
