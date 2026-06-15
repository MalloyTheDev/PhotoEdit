#include "CanvasView.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>

namespace pe::app {

CanvasView::CanvasView(QWidget* parent) : QWidget(parent) {
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

QPoint CanvasView::imageOrigin() const noexcept {
    if (image_.isNull()) return QPoint(0, 0);
    return QPoint((width() - image_.width()) / 2, (height() - image_.height()) / 2);
}

pe::StrokePoint CanvasView::sampleAt(QPointF widgetPos) const {
    const QPoint o = imageOrigin();
    // Mouse input carries no pressure; tablet pressure/tilt arrive in a later pass.
    return pe::StrokePoint{
        {static_cast<float>(widgetPos.x() - o.x()), static_cast<float>(widgetPos.y() - o.y())},
        1.0f};
}

void CanvasView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(48, 48, 48));  // neutral pasteboard
    if (image_.isNull()) return;
    painter.drawImage(imageOrigin(), image_);  // composite at 100%, centered
}

void CanvasView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || doc_ == nullptr) {
        QWidget::mousePressEvent(e);
        return;
    }
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
    if (!tool_.isStroking() || doc_ == nullptr) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    tool_.extend(*doc_, sampleAt(e->position()));
    refreshImage();  // live preview repaint (bypasses the observer)
    update();
}

void CanvasView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !tool_.isStroking()) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    tool_.end(*doc_);  // commits one undoable command; the observer refreshes us
    refreshImage();    // also covers a stroke that deposited nothing (no notify)
    update();
}

}  // namespace pe::app
