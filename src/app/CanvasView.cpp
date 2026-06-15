#include "CanvasView.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <QColor>
#include <QPainter>

namespace pe::app {

CanvasView::CanvasView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
}

void CanvasView::showDocument(const pe::Document* doc) {
    if (doc == nullptr) {
        image_ = QImage();
    } else {
        const pe::PixelBuffer buf = doc->compositeImage();
        if (buf.isEmpty()) {
            image_ = QImage();
        } else {
            // Rgba8 is R,G,B,A bytes == QImage::Format_RGBA8888. The wrapping QImage
            // references the temporary buffer, so copy() to own the pixels.
            const QImage view(reinterpret_cast<const uchar*>(buf.data()), buf.width(), buf.height(),
                              buf.width() * 4, QImage::Format_RGBA8888);
            image_ = view.copy();
        }
    }
    updateGeometry();
    update();
}

QSize CanvasView::sizeHint() const {
    return image_.isNull() ? QSize(640, 480) : image_.size();
}

void CanvasView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(48, 48, 48));  // neutral pasteboard
    if (image_.isNull()) return;
    // Center the composite at 100% (zoom/scroll arrive with the real viewport).
    const int x = (width() - image_.width()) / 2;
    const int y = (height() - image_.height()) / 2;
    painter.drawImage(x, y, image_);
}

}  // namespace pe::app
