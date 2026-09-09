#include "SwatchesPanel.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRect>

#include <algorithm>
#include <utility>

namespace pe::app {

namespace {

constexpr int kChip = 18;    // chip edge, device-independent pixels
constexpr int kGap = 3;      // space between chips
constexpr int kMargin = 6;   // space around the grid
constexpr int kMinCols = 4;  // never collapse to a single column, however narrow the dock

// The default palette: the greys, then a hue wheel at three lightnesses. Not a copy of any
// other application's set; it is the smallest palette that is actually useful for painting,
// which is a full spread of hues plus the neutrals people reach for first.
QVector<QColor> defaultPalette() {
    QVector<QColor> out;
    for (int i = 0; i < 8; ++i) {
        const int v = i * 255 / 7;
        out.append(QColor(v, v, v));
    }
    for (const int value : {255, 190, 120}) {
        for (int h = 0; h < 360; h += 30) out.append(QColor::fromHsv(h, 255, value));
    }
    return out;
}

}  // namespace

SwatchesPanel::SwatchesPanel(QWidget* parent) : QWidget(parent), colors_(defaultPalette()) {
    setObjectName(QStringLiteral("SwatchesPanel"));
    setFocusPolicy(Qt::StrongFocus);  // arrow keys and Space, not mouse only
    setAccessibleName(QStringLiteral("Swatches"));
    setToolTip(QStringLiteral("Click a swatch to make it the foreground colour"));
}

void SwatchesPanel::setColors(QVector<QColor> colors) {
    colors_ = std::move(colors);
    current_ = -1;
    updateGeometry();
    update();
}

int SwatchesPanel::columns() const {
    const int usable = width() - 2 * kMargin + kGap;
    return std::max(kMinCols, usable / (kChip + kGap));
}

QRect SwatchesPanel::chipRect(int index) const {
    if (index < 0 || index >= colors_.size()) return QRect();
    const int cols = columns();
    const int col = index % cols;
    const int row = index / cols;
    return QRect(kMargin + col * (kChip + kGap), kMargin + row * (kChip + kGap), kChip, kChip);
}

int SwatchesPanel::chipAt(QPoint pos) const {
    for (int i = 0; i < colors_.size(); ++i) {
        if (chipRect(i).contains(pos)) return i;
    }
    return -1;
}

void SwatchesPanel::setCurrentColor(const QColor& c) {
    int found = -1;
    for (int i = 0; i < colors_.size(); ++i) {
        if (colors_[i].rgb() == c.rgb()) {
            found = i;
            break;
        }
    }
    if (found == current_) return;
    current_ = found;
    update();
}

void SwatchesPanel::choose(int index) {
    if (index < 0 || index >= colors_.size()) return;
    current_ = index;
    update();
    emit colorChosen(colors_[index]);
}

QSize SwatchesPanel::sizeHint() const {
    const int cols = std::max(kMinCols, columns());
    const int rows = (colors_.size() + cols - 1) / std::max(1, cols);
    return QSize(2 * kMargin + cols * (kChip + kGap) - kGap,
                 2 * kMargin + rows * (kChip + kGap) - kGap);
}

void SwatchesPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    for (int i = 0; i < colors_.size(); ++i) {
        const QRect r = chipRect(i);
        p.fillRect(r, colors_[i]);
        // A dark hairline round every chip, so a white swatch still has an edge against the
        // panel and two adjacent light swatches do not merge into one block.
        p.setPen(QPen(QColor(0, 0, 0, 120), 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        if (i == current_) {
            // The current colour, marked white-over-black for the same reason the marching
            // ants are: the ring has to read on a chip of any colour, including both.
            p.setPen(QPen(QColor(0, 0, 0), 3));
            p.drawRect(r.adjusted(-2, -2, 1, 1));
            p.setPen(QPen(QColor(255, 255, 255), 1));
            p.drawRect(r.adjusted(-2, -2, 1, 1));
        }
    }
    if (hasFocus() && current_ >= 0) {
        p.setPen(QPen(palette().highlight().color(), 1, Qt::DotLine));
        p.drawRect(chipRect(current_).adjusted(-4, -4, 3, 3));
    }
}

void SwatchesPanel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    const int i = chipAt(e->position().toPoint());
    if (i < 0) {
        QWidget::mousePressEvent(e);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    choose(i);
}

void SwatchesPanel::keyPressEvent(QKeyEvent* e) {
    // Reachable without a mouse. A grid of coloured squares is exactly the sort of control
    // that ends up mouse-only, and then the palette is unusable from the keyboard entirely.
    if (colors_.isEmpty()) {
        QWidget::keyPressEvent(e);
        return;
    }
    const int cols = columns();
    int next = current_ < 0 ? 0 : current_;
    switch (e->key()) {
        case Qt::Key_Left:
            next = current_ < 0 ? 0 : current_ - 1;
            break;
        case Qt::Key_Right:
            next = current_ < 0 ? 0 : current_ + 1;
            break;
        case Qt::Key_Up:
            next = current_ < 0 ? 0 : current_ - cols;
            break;
        case Qt::Key_Down:
            next = current_ < 0 ? 0 : current_ + cols;
            break;
        case Qt::Key_Home:
            next = 0;
            break;
        case Qt::Key_End:
            next = static_cast<int>(colors_.size()) - 1;
            break;
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            choose(next);  // commit the highlighted chip
            return;
        default:
            QWidget::keyPressEvent(e);
            return;
    }
    if (next < 0 || next >= colors_.size()) return;  // at an edge: stay put rather than wrap
    choose(next);
}

}  // namespace pe::app
