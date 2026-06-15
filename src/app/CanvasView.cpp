#include "CanvasView.hpp"

#include "Theme.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

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
        // Abandon any in-progress stroke on the outgoing document, reverting its
        // live preview, so the tool never carries stroke state across documents.
        if (tool_.isStroking()) tool_.cancel(*doc_);
        doc_->removeObserver(this);
    }
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    refreshImage();
    needsFit_ = true;  // fit the new document once we have a valid widget size
    maybeInitialFit();
    updateGeometry();
    update();
}

void CanvasView::onDocumentChanged(const pe::Document&, const pe::DocumentChange&) {
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
    setCursor(t == Tool::Hand       ? Qt::OpenHandCursor
              : t == Tool::Zoom     ? Qt::PointingHandCursor
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
    if (toolMode_ != Tool::Brush && toolMode_ != Tool::Eraser) return;  // Inactive: no paint
    // Recover if a prior stroke never received its release (e.g. mouse capture was
    // stolen by a modal/Alt-Tab): drop the stale preview before starting fresh.
    if (tool_.isStroking()) tool_.cancel(*doc_);
    // Begin a stroke; the first dab is a live preview the observer won't see, so
    // refresh explicitly. begin() fails when there is no paintable active layer.
    if (tool_.begin(*doc_, sampleAt(e->position()))) {
        refreshImage();
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
    if (!tool_.isStroking() || doc_ == nullptr) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    tool_.extend(*doc_, sampleAt(e->position()));
    refreshImage();  // live preview repaint (bypasses the observer)
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
