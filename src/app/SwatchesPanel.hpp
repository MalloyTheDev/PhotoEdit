#pragma once

#include <QColor>
#include <QSize>
#include <QVector>
#include <QWidget>

namespace pe::app {

// A grid of colour chips: click one to make it the foreground colour.
//
// The panel that used to sit here was a centred label reading "Swatches", which is
// indistinguishable from a panel that is broken. This one does the job a swatch grid does
// and nothing more: it is a palette, not a colour editor. Editing lives in the Color panel
// next to it, and this pushes into that through MainWindow, which owns the foreground.
//
// Pure Qt, like ColorPanel: it knows nothing about the document or the engine.
class SwatchesPanel : public QWidget {
    Q_OBJECT

public:
    explicit SwatchesPanel(QWidget* parent = nullptr);

    // The palette, in row-major order. Replacing it keeps the widget usable with a loaded
    // palette later; the default is the built-in set.
    [[nodiscard]] const QVector<QColor>& colors() const noexcept { return colors_; }
    void setColors(QVector<QColor> colors);

    // The chip under a widget point, or -1. Public because "clicking a chip picks THAT
    // colour" is the whole contract, and it cannot be asserted without agreeing where the
    // chips are.
    [[nodiscard]] int chipAt(QPoint pos) const;

    // Which chip is marked as current, or -1 when the foreground matches none of them.
    [[nodiscard]] int currentIndex() const noexcept { return current_; }
    // Mark the chip matching `c`, if any. Does not emit; MainWindow calls this to keep the
    // grid in step with a foreground set from anywhere else.
    void setCurrentColor(const QColor& c);

    [[nodiscard]] QSize sizeHint() const override;

signals:
    void colorChosen(const QColor& color);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    [[nodiscard]] int columns() const;
    [[nodiscard]] QRect chipRect(int index) const;
    void choose(int index);

    QVector<QColor> colors_;
    int current_ = -1;
};

}  // namespace pe::app
