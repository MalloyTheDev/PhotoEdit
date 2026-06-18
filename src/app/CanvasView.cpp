#include "CanvasView.hpp"

#include "Theme.hpp"
#include "pe/core/Brush.hpp"  // pe::PaintCommand (move-tool preview command)
#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Commands.hpp"
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
    renderer_.reset();  // unregister the renderer's observer before doc_ goes away
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void CanvasView::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) {
        // Abandon any in-progress stroke or move on the outgoing document, reverting its
        // live preview, so a tool never carries provisional state across documents.
        if (tool_.isStroking()) tool_.cancel(*doc_);
        cancelMovePreview();
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
    doc_ = doc;
    if (doc_ != nullptr) {
        doc_->addObserver(this);
        renderer_ = std::make_unique<pe::CanvasRenderer>(*doc_);  // observes doc_ itself
    }
    selectionAnts_ = doc_ != nullptr ? doc_->selection().tightBounds() : pe::Rect{};
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
    // A committed mutation (paint commit, undo/redo, file load). The renderer is also an
    // observer and has already marked the changed tiles dirty, so we only need to repaint;
    // paintEvent recomposites just those tiles. The live brush preview repaints separately.
    update();
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

void CanvasView::cancelMovePreview() {
    if (movePreview_ && doc_ != nullptr) movePreview_->undo(*doc_);  // restore the layer
    movePreview_.reset();
    movingContent_ = false;
    moveLayer_ = pe::kNoLayer;
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
    toolMode_ = t;
    if (t == Tool::Brush) {
        tool_.setMode(pe::PaintToolController::Mode::Brush);
    } else if (t == Tool::Eraser) {
        tool_.setMode(pe::PaintToolController::Mode::Eraser);
    } else if (doc_ != nullptr && tool_.isStroking()) {
        tool_.cancel(*doc_);  // switching away from a paint tool drops any live stroke
        reloadImage();        // the cancel reverted tiles without notifying the renderer
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
    if (draggingLasso_) {  // switching away mid-lasso discards the in-progress path
        draggingLasso_ = false;
        lassoPts_.clear();
        update();
    }
    if (draggingGradient_) {  // switching away mid-gradient discards the in-progress guide
        draggingGradient_ = false;
        update();
    }
    setCursor(t == Tool::Hand       ? Qt::OpenHandCursor
              : t == Tool::Zoom     ? Qt::PointingHandCursor
              : t == Tool::Move     ? Qt::SizeAllCursor
              : t == Tool::Wand     ? Qt::PointingHandCursor
              : t == Tool::Type     ? Qt::IBeamCursor
              : t == Tool::Inactive ? Qt::ArrowCursor
                                    : Qt::CrossCursor);  // all other interactive tools
                                                         // (Brush/Eraser/Marquee/Lasso/Crop/
                                                         // Bucket/Gradient/Eyedropper)
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

void CanvasView::paintEvent(QPaintEvent*) {
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

    // Composite ONLY the visible canvas region through the tile cache: a huge canvas never
    // overflows the whole-image composite budget, and panning recomposites just the
    // newly-exposed tiles. (At extreme zoom-out, a >64 MP visible region exceeds the
    // renderer's per-call budget and renderRegion returns empty — a known limitation pending
    // mipmapped display; the canvas simply shows the pasteboard until zoomed back in.)
    const pe::Rect canvas{0, 0, cs.width, cs.height};
    const pe::Rect vis = view_.visibleDocRect(pe::Size{width(), height()}).intersected(canvas);
    if (!vis.isEmpty()) {
        // Keep the cache at least as large as the visible tile span (plus a one-viewport pan
        // margin) so no visible tile evicts another mid-frame — otherwise a zoomed-out view
        // spanning more than the default budget would recomposite every tile every paint.
        const std::size_t visTiles = static_cast<std::size_t>(pe::tilesForRect(vis).count());
        renderer_->setCacheBudgetTiles(
            std::max<std::size_t>(pe::kDefaultDisplayCacheTiles, visTiles * 2 + 16));
        const pe::PixelBuffer buf = renderer_->renderRegion(vis);  // alive through drawImage
        if (!buf.isEmpty()) {
            const QImage img(reinterpret_cast<const uchar*>(buf.data()), buf.width(), buf.height(),
                             buf.width() * 4, QImage::Format_RGBA8888);
            painter.drawImage(QPointF(vis.x, vis.y), img);
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
        } else {
            drawAntRect(selectionAnts_);
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
                if (renderer_ != nullptr) renderer_->invalidate(tool_.strokeDirtyBounds());
                update();
            }
            break;
        case QEvent::TabletMove:
            if (tool_.isStroking()) {
                tool_.extend(*doc_, sp);
                if (renderer_ != nullptr) renderer_->invalidate(tool_.strokeDirtyBounds());
                update();
            }
            break;
        case QEvent::TabletRelease:
            if (tool_.isStroking()) {
                const pe::Rect dirty = tool_.strokeDirtyBounds();  // capture before end() clears it
                tool_.end(*doc_);
                if (renderer_ != nullptr) renderer_->invalidate(dirty);  // covers no-deposit too
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
        cancelMovePreview();  // recover from any stale (capture-lost) preview
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
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        const pe::PixelBuffer buf = doc_->compositeImage();  // sample the composited canvas
        if (!buf.isEmpty()) {
            constexpr int kWandTolerance = 32;  // default per-channel tolerance (v1)
            pe::Selection sel =
                pe::magicWandSelection(buf, static_cast<int>(std::lround(d.x)),
                                       static_cast<int>(std::lround(d.y)), kWandTolerance);
            if (sel.active()) doc_->history().push(std::make_unique<SetSelectionCommand>(sel));
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
        // Report the click in document space; MainWindow prompts for the text and stamps it
        // (rasterized) onto the active layer as an undoable command.
        const pe::PointD d = view_.viewToDoc(pe::PointD{e->position().x(), e->position().y()});
        emit textRequested(QPointF(d.x, d.y));
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
        if (renderer_ != nullptr) renderer_->invalidate(tool_.strokeDirtyBounds());  // first dab
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
        reloadImage();  // preview applied without notifying: drop the cache + repaint
        return;
    }
    if (!tool_.isStroking() || doc_ == nullptr) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    tool_.extend(*doc_, sampleAt(e->position()));
    if (renderer_ != nullptr) renderer_->invalidate(tool_.strokeDirtyBounds());  // stroke footprint
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
                doc_->history().push(std::make_unique<SetSelectionCommand>(target));
                // command will execute, snapshot old, notify
            }
        }
        liveMarquee_ = Rect{};
        update();
        return;
    }
    if (draggingLasso_ && doc_ != nullptr) {
        draggingLasso_ = false;
        if (lassoPts_.size() >= 3) {
            Selection target;
            target.selectPolygon(lassoPts_);
            if (target.active()) {
                doc_->history().push(std::make_unique<SetSelectionCommand>(target));
            }
        }
        lassoPts_.clear();
        update();
        return;
    }
    if (draggingGradient_) {
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
    const pe::Rect dirty = tool_.strokeDirtyBounds();  // capture before end() clears it
    tool_.end(*doc_);  // commits one undoable command; the renderer-observer marks it dirty
    if (renderer_ != nullptr) renderer_->invalidate(dirty);  // also covers a no-deposit stroke
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
