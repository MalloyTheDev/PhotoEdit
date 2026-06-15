#pragma once

#include <QImage>
#include <QWidget>

namespace pe {
class Document;
}

namespace pe::app {

// The central canvas: shows a document's flattened composite. This is the minimal
// raster view; the tiled/zoomable viewport (docs/systems/02-canvas-rendering.md)
// replaces it later. Pass nullptr to clear.
class CanvasView : public QWidget {
    Q_OBJECT

public:
    explicit CanvasView(QWidget* parent = nullptr);

    // Re-flatten `doc` (or clear if null) and repaint.
    void showDocument(const pe::Document* doc);

protected:
    void paintEvent(QPaintEvent*) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    QImage image_;  // a private copy of the current composite (RGBA8888)
};

}  // namespace pe::app
