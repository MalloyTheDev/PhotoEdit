#include "CanvasView.hpp"

#include "Theme.hpp"
#include "pe/core/Brush.hpp"  // pe::PaintCommand (move-tool preview command)
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"  // pe::moveLayerContent
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Selection.hpp"

#include <QApplication>
#include <QColor>
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
#include <cstring>

namespace pe::app {

namespace {
constexpr double kZoomStep = 1.25;  // per Zoom In/Out and per wheel notch

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
    // A visible default: an opaque black, medium round tip.
    pe::BrushSettings b = tool_.brush();
    b.diameter = 24.0f;
    tool_.setBrush(b);
}

CanvasView::~CanvasView() {
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void CanvasView::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) {
        // Abandon any in-progress stroke or move on the outgoing document, reverting its
        // live preview, so a tool never carries provisional state across documents.
        if (tool_.isStroking()) tool_.cancel(*doc_);
        cancelMovePreview();
        doc_->removeObserver(this);
    }
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    selectionAnts_ = doc_ != nullptr ? doc_->selection().tightBounds() : pe::Rect{};
    refreshImage();
    needsFit_ = true;  // fit the new document once we have a valid widget size
    maybeInitialFit();
    updateGeometry();
    update();
}

void CanvasView::onDocumentChanged(const pe::Document&, const pe::DocumentChange& ch) {
    if (ch.kind == pe::DocumentChange::Kind::Selection) {
        // Selection change only affects the marching-ants overlay, not pixel content.
        // Recompute the pixel-tight ants bounds here (once per change) so paintEvent never
        // scans the selection mask per frame.
        selectionAnts_ = doc_ != nullptr ? doc_->selection().tightBounds() : pe::Rect{};
        update();
        return;
    }
    // A committed mutation (paint commit, undo/redo, file load). The live brush
    // preview drives its own repaint and deliberately does not route through here.
    refreshImage();
    update();
}

void CanvasView::refreshImage() {
    if (doc_ == nullptr) {
        image_ = QImage();
        return;
    }
    const pe::PixelBuffer buf = doc_->compositeImage();
    if (buf.isEmpty()) {
        image_ = QImage();
        return;
    }
    // Rgba8 is R,G,B,A bytes == QImage::Format_RGBA8888. The wrapping QImage
    // references the temporary buffer, so copy() to own the pixels.
    const QImage view(reinterpret_cast<const uchar*>(buf.data()), buf.width(), buf.height(),
                      buf.width() * 4, QImage::Format_RGBA8888);
    image_ = view.copy();
}

void CanvasView::reloadImage() {
    refreshImage();
    update();
}

void CanvasView::cancelMovePreview() {
    if (movePreview_ && doc_ != nullptr) movePreview_->undo(*doc_);  // restore the layer
    movePreview_.reset();
    movingContent_ = false;
    moveLayer_ = pe::kNoLayer;
}

void CanvasView::refreshRegion(const pe::Rect& region) {
    // No live full-canvas image to patch (no document, or a canvas too large to
    // flatten so refreshImage() left image_ null): re-run the full path, which
    // handles those cases coherently.
    if (doc_ == nullptr || image_.isNull()) {
        refreshImage();
        return;
    }
    // Clamp to the canvas: the stroke's dirty bounds are tile-granular and may
    // overhang the document edge.
    const pe::Rect canvas{0, 0, image_.width(), image_.height()};
    const pe::Rect r = region.intersected(canvas);
    if (r.isEmpty()) return;  // nothing on-canvas changed

    const pe::PixelBuffer buf = pe::compositeToImage(doc_->topLevelLayers(), r);
    if (buf.isEmpty() || buf.width() != r.width || buf.height() != r.height) {
        refreshImage();  // region exceeded the composite budget, or a size mismatch
        return;
    }
    // Patch image_ in place, row by row. image_ is Format_RGBA8888 and buf is a
    // PixelBuffer of Rgba8 (R,G,B,A bytes), the same straight-alpha byte layout, so a
    // raw per-row copy is exact.
    constexpr int kBpp = 4;
    const auto* src = reinterpret_cast<const uchar*>(buf.data());
    for (int y = 0; y < r.height; ++y) {
        const uchar* srcRow = src + static_cast<std::size_t>(y) * buf.width() * kBpp;
        uchar* dstRow = image_.scanLine(r.y + y) + static_cast<std::ptrdiff_t>(r.x) * kBpp;
        std::memcpy(dstRow, srcRow, static_cast<std::size_t>(r.width) * kBpp);
    }
}

QSize CanvasView::sizeHint() const {
    return image_.isNull() ? QSize(640, 480) : image_.size();
}

void CanvasView::fitToWindow() {
    if (doc_ == nullptr || image_.isNull()) return;
    const double w = image_.width();
    const double h = image_.height();
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
    if (doc_ == nullptr || image_.isNull()) return;
    needsFit_ = false;
    view_.setRotation(0.0);
    view_.setZoom(1.0);
    view_.setFocus(pe::PointD{image_.width() / 2.0, image_.height() / 2.0},
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
    toolMode_ = t;
    if (t == Tool::Brush) {
        tool_.setMode(pe::PaintToolController::Mode::Brush);
    } else if (t == Tool::Eraser) {
        tool_.setMode(pe::PaintToolController::Mode::Eraser);
    } else if (doc_ != nullptr && tool_.isStroking()) {
        tool_.cancel(*doc_);  // switching away from a paint tool drops any live stroke
        refreshImage();
        update();
    }
    if (draggingMarquee_ && doc_ != nullptr) {
        draggingMarquee_ = false;
        liveMarquee_ = Rect{};
        update();
    }
    if (movingContent_) {  // switching away from Move drops any live move preview
        cancelMovePreview();
        reloadImage();
    }
    setCursor(t == Tool::Hand       ? Qt::OpenHandCursor
              : t == Tool::Zoom     ? Qt::PointingHandCursor
              : t == Tool::Move     ? Qt::SizeAllCursor
              : t == Tool::Marquee  ? Qt::CrossCursor
              : t == Tool::Inactive ? Qt::ArrowCursor
                                    : Qt::CrossCursor);
}

void CanvasView::maybeInitialFit() {
    if (needsFit_ && doc_ != nullptr && !image_.isNull() && width() > 0 && height() > 0) {
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

void CanvasView::paintEvent(QPaintEvent*) {
    maybeInitialFit();

    QPainter painter(this);
    painter.fillRect(rect(), themeColors(currentTheme()).canvas);  // themed pasteboard
    if (image_.isNull()) return;

    // Device-space rectangle the document maps to (rotation is not exposed yet, so
    // doc->view is scale + translate and the image stays axis-aligned).
    const pe::PointD tl = view_.docToView(pe::PointD{0.0, 0.0});
    const pe::PointD br = view_.docToView(
        pe::PointD{static_cast<double>(image_.width()), static_cast<double>(image_.height())});
    const QRectF devRect(QPointF(tl.x, tl.y), QPointF(br.x, br.y));

    painter.fillRect(devRect, checker_);  // transparency shows through as a checkerboard

    // Crisp pixels when magnifying, smooth when minifying.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, view_.zoom() < 1.0);
    painter.setTransform(toQTransform(view_.docToView()));
    painter.drawImage(QPointF(0.0, 0.0), image_);

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

        // Live drag rect (while marquee tool dragging) takes precedence visually; otherwise
        // the committed selection's pixel-tight outline (cached on the last selection change,
        // so the per-pixel scan does not run every repaint).
        if (draggingMarquee_ && liveMarquee_.width > 0 && liveMarquee_.height > 0) {
            drawAntRect(liveMarquee_);
        } else {
            drawAntRect(selectionAnts_);
        }
    }
}

void CanvasView::tabletEvent(QTabletEvent* e) {
    if (doc_ == nullptr) {
        e->ignore();
        return;
    }
    if (toolMode_ != Tool::Brush && toolMode_ != Tool::Eraser) {
        e->ignore();
        return;
    }
    pe::StrokePoint sp = sampleAt(e->position());
    sp.pressure = std::clamp(static_cast<float>(e->pressure()), 0.0f, 1.0f);
    switch (e->type()) {
        case QEvent::TabletPress:
            if (tool_.isStroking()) tool_.cancel(*doc_);
            if (tool_.begin(*doc_, sp, &doc_->selection())) {
                refreshRegion(tool_.strokeDirtyBounds());
                update();
            }
            break;
        case QEvent::TabletMove:
            if (tool_.isStroking()) {
                tool_.extend(*doc_, sp);
                refreshRegion(tool_.strokeDirtyBounds());
                update();
            }
            break;
        case QEvent::TabletRelease:
            if (tool_.isStroking()) {
                tool_.end(*doc_);
                refreshImage();
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
        panning_ = true;
        lastPanPos_ = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || doc_ == nullptr) {
        QWidget::mousePressEvent(e);
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
    if (toolMode_ == Tool::Marquee) {
        draggingMarquee_ = true;
        marqueeAnchor_ = e->position();
        liveMarquee_ = Rect{};
        update();
        return;
    }
    if (toolMode_ == Tool::Eyedropper && doc_ != nullptr && !image_.isNull()) {
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const int ix = std::clamp(static_cast<int>(std::lround(d.x)), 0, image_.width() - 1);
        const int iy = std::clamp(static_cast<int>(std::lround(d.y)), 0, image_.height() - 1);
        emit colorPicked(image_.pixelColor(ix, iy));
        return;
    }
    if (toolMode_ == Tool::Move) {
        cancelMovePreview();  // recover from any stale (capture-lost) preview
        movingContent_ = true;
        moveStartWidget_ = e->position();
        moveLayer_ = doc_->activeLayer();  // move the layer that is active when the drag begins
        return;
    }
    if (toolMode_ != Tool::Brush && toolMode_ != Tool::Eraser) return;  // Inactive: no paint
    // Recover if a prior stroke never received its release (e.g. mouse capture was
    // stolen by a modal/Alt-Tab): drop the stale preview before starting fresh.
    if (tool_.isStroking()) tool_.cancel(*doc_);
    // Begin a stroke; the first dab is a live preview the observer won't see, so
    // refresh explicitly. begin() fails when there is no paintable active layer.
    // Pass the document selection so edits are gated.
    if (tool_.begin(*doc_, sampleAt(e->position()), &doc_->selection())) {
        refreshRegion(tool_.strokeDirtyBounds());  // only the first dab's footprint
        update();
    }
}

void CanvasView::mouseMoveEvent(QMouseEvent* e) {
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
    if (movingContent_ && doc_ != nullptr) {
        // Cumulative drag delta in document pixels, applied to the ORIGINAL content each move
        // (the prior preview is reverted first), so the result equals one move by the total.
        const pe::PointD a =
            view_.viewToDoc(pe::PointD{moveStartWidget_.x(), moveStartWidget_.y()});
        const pe::PointD b = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const int dx = static_cast<int>(std::lround(b.x - a.x));
        const int dy = static_cast<int>(std::lround(b.y - a.y));
        if (movePreview_) {
            movePreview_->undo(*doc_);
            movePreview_.reset();
        }
        movePreview_ = pe::moveLayerContent(*doc_, moveLayer_, dx, dy);
        if (movePreview_) movePreview_->execute(*doc_);
        reloadImage();  // a move can touch a large region; re-flatten the whole canvas
        return;
    }
    if (!tool_.isStroking() || doc_ == nullptr) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    tool_.extend(*doc_, sampleAt(e->position()));
    refreshRegion(tool_.strokeDirtyBounds());  // live preview, stroke footprint only
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
    if (draggingMarquee_ && doc_ != nullptr) {
        draggingMarquee_ = false;
        if (liveMarquee_.width > 0 && liveMarquee_.height > 0) {
            Selection target = doc_->selection();
            Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
            if (mods & Qt::ShiftModifier) {
                target.addRect(liveMarquee_);
            } else if (mods & (Qt::AltModifier | Qt::ControlModifier)) {
                target.subtractRect(liveMarquee_);
            } else {
                target.selectRect(liveMarquee_);
            }
            doc_->history().push(std::make_unique<SetSelectionCommand>(target));
            // command will execute, snapshot old, notify
        }
        liveMarquee_ = Rect{};
        update();
        return;
    }
    if (movingContent_) {
        movingContent_ = false;
        moveLayer_ = pe::kNoLayer;
        if (movePreview_ && doc_ != nullptr) {
            movePreview_->undo(*doc_);  // back to the pre-move state
            doc_->history().push(
                std::move(movePreview_));  // commit one undo step; observer refreshes
        } else {
            reloadImage();  // dragged back to origin / nothing movable: ensure a clean canvas
        }
        return;
    }
    if (e->button() != Qt::LeftButton || !tool_.isStroking()) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    tool_.end(*doc_);  // commits one undoable command; the observer refreshes us
    refreshImage();    // also covers a stroke that deposited nothing (no notify)
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
