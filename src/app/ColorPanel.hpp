#pragma once

#include <QColor>
#include <QImage>
#include <QWidget>

class QLabel;

namespace pe::app {

// A self-contained HSV colour picker: a 2D saturation/value square for the current
// hue (saturation across, value up the screen), a full-rainbow hue strip beneath it,
// and a swatch + hex readout at the bottom. Pure Qt — it knows nothing about the
// document or engine; it just owns a QColor and announces edits via colorChanged().
//
// MainWindow wires colorChanged() to the active brush/foreground colour and calls
// setColor() to push the foreground swatch back into the picker without echoing.
class ColorPanel : public QWidget {
    Q_OBJECT

public:
    explicit ColorPanel(QWidget* parent = nullptr);

    // The current colour. Always fully opaque; the picker edits hue/sat/value only.
    [[nodiscard]] QColor color() const;

    // Set the colour from the outside (e.g. syncing from the foreground swatch).
    // Updates the UI but deliberately does not emit colorChanged().
    void setColor(const QColor& c);

signals:
    void colorChanged(const QColor& color);  // emitted whenever the user edits the colour

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    // The two interactive regions, laid out top-to-bottom inside the widget.
    [[nodiscard]] QRect svRect() const;   // saturation/value square
    [[nodiscard]] QRect hueRect() const;  // rainbow hue strip

    void rebuildSvImage();   // repaint the SV square for the current hue (hue changes only)
    void rebuildHueImage();  // build the rainbow strip once; it never depends on the colour

    // Route a mouse point to whichever region the gesture began in, update the colour,
    // and emit colorChanged(). `region` pins drags to the press target.
    void applyPointer(const QPoint& pos);

    void syncSwatch();  // refresh the swatch fill + hex label from the current colour

    // Hue/sat/value in [0,1]; the canonical state the views are derived from.
    float hue_ = 0.0f;
    float sat_ = 1.0f;
    float val_ = 1.0f;

    // Which region the active drag is editing (set on press), so a drag that leaves
    // one region keeps editing it rather than jumping to the other.
    enum class Region { None, SatVal, Hue };
    Region drag_ = Region::None;

    QImage svImage_;   // cached SV square pixels for hue_
    QImage hueImage_;  // cached rainbow strip
    QLabel* hex_ = nullptr;
    QLabel* swatch_ = nullptr;
};

}  // namespace pe::app
