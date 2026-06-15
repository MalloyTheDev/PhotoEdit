#include "ColorPanel.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

namespace pe::app {

namespace {
constexpr int kHueHeight = 18;  // height of the rainbow strip
constexpr int kGap = 6;         // spacing between the square, strip, and readout
constexpr int kSwatch = 22;     // edge of the current-colour swatch

// Clamp a value into [lo, hi]; the picker maps freely-dragged mouse coords back into
// the [0,1] colour ranges, so out-of-rect drags pin to the edge rather than wrap.
[[nodiscard]] float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

[[nodiscard]] QString hexOf(const QColor& c) {
    return c.name(QColor::HexRgb).toUpper();  // "#RRGGBB"
}
}  // namespace

ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent) {
    setMinimumSize(220, 180);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(kGap);

    // The picker itself is custom-painted in paintEvent; reserve the space for it by
    // letting this widget stretch, and put only the swatch row in the layout.
    root->addStretch(1);

    auto* row = new QHBoxLayout();
    row->setSpacing(kGap);
    swatch_ = new QLabel(this);
    swatch_->setFixedSize(kSwatch, kSwatch);
    swatch_->setFrameShape(QFrame::Box);  // a thin border around the colour chip
    hex_ = new QLabel(this);
    hex_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(swatch_);
    row->addWidget(hex_, 1);
    root->addLayout(row);

    syncSwatch();
}

QColor ColorPanel::color() const {
    return QColor::fromHsvF(hue_, sat_, val_);
}

void ColorPanel::setColor(const QColor& c) {
    // Take hue/sat/value from the incoming colour. A grey/black input reports hue -1
    // from Qt; preserve the current hue so the SV square doesn't snap to red.
    float h = static_cast<float>(c.hsvHueF());
    if (h < 0.0f) h = hue_;
    hue_ = clamp01(h);
    sat_ = clamp01(static_cast<float>(c.hsvSaturationF()));
    val_ = clamp01(static_cast<float>(c.valueF()));
    svImage_ = QImage();  // invalidate the cached square; rebuilt lazily on paint
    syncSwatch();
    update();
}

QRect ColorPanel::svRect() const {
    // The square fills the width above the hue strip and the swatch row, leaving gaps.
    const int bottom = height() - kSwatch - kGap - kHueHeight - 2 * kGap;
    const int h = std::max(0, bottom);
    return QRect(0, 0, width(), h);
}

QRect ColorPanel::hueRect() const {
    const QRect sv = svRect();
    return QRect(0, sv.bottom() + 1 + kGap, width(), kHueHeight);
}

void ColorPanel::rebuildSvImage() {
    const QRect r = svRect();
    if (r.width() <= 0 || r.height() <= 0) {
        svImage_ = QImage();
        return;
    }
    // Per-pixel fill: x -> saturation (0..1), y -> value (1 at top, 0 at bottom). Only
    // rebuilt when the hue or the square's size changes, so the cost stays bounded.
    QImage img(r.size(), QImage::Format_RGB32);
    for (int y = 0; y < r.height(); ++y) {
        const float v = 1.0f - static_cast<float>(y) / static_cast<float>(r.height() - 1);
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < r.width(); ++x) {
            const float s = static_cast<float>(x) / static_cast<float>(r.width() - 1);
            line[x] = QColor::fromHsvF(hue_, clamp01(s), clamp01(v)).rgb();
        }
    }
    svImage_ = img;
}

void ColorPanel::rebuildHueImage() {
    const QRect r = hueRect();
    if (r.width() <= 0) {
        hueImage_ = QImage();
        return;
    }
    // One fully-saturated rainbow column per x; constant down each column, so paint a
    // single row and let drawImage stretch it vertically.
    QImage img(r.width(), 1, QImage::Format_RGB32);
    auto* line = reinterpret_cast<QRgb*>(img.scanLine(0));
    for (int x = 0; x < r.width(); ++x) {
        const float h = static_cast<float>(x) / static_cast<float>(r.width() - 1);
        line[x] = QColor::fromHsvF(clamp01(h), 1.0f, 1.0f).rgb();
    }
    hueImage_ = img;
}

void ColorPanel::paintEvent(QPaintEvent*) {
    if (svImage_.isNull() || svImage_.size() != svRect().size()) rebuildSvImage();
    if (hueImage_.isNull() || hueImage_.width() != hueRect().width()) rebuildHueImage();

    QPainter p(this);
    const QRect sv = svRect();
    const QRect hue = hueRect();

    if (!svImage_.isNull()) p.drawImage(sv.topLeft(), svImage_);
    if (!hueImage_.isNull()) p.drawImage(hue, hueImage_);  // stretched to the strip height

    // SV cursor: a ring at (saturation, value), clamped a couple px inside the square.
    if (sv.height() > 0) {
        const int cx = sv.left() + std::lround(sat_ * (sv.width() - 1));
        const int cy = sv.top() + std::lround((1.0f - val_) * (sv.height() - 1));
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPoint(cx, cy), 5, 5);
        p.setPen(QPen(Qt::white, 1));
        p.drawEllipse(QPoint(cx, cy), 4, 4);
    }

    // Hue marker: a vertical bar at the current hue's column.
    const int hx = hue.left() + std::lround(hue_ * (hue.width() - 1));
    p.setPen(QPen(Qt::white, 1));
    p.drawLine(hx, hue.top(), hx, hue.bottom());
    p.setPen(QPen(Qt::black, 1));
    p.drawLine(hx - 1, hue.top(), hx - 1, hue.bottom());
    p.drawLine(hx + 1, hue.top(), hx + 1, hue.bottom());
}

void ColorPanel::applyPointer(const QPoint& pos) {
    const QRect sv = svRect();
    const QRect hue = hueRect();

    if (drag_ == Region::SatVal && sv.width() > 1 && sv.height() > 1) {
        const int x = std::clamp(pos.x(), sv.left(), sv.right());
        const int y = std::clamp(pos.y(), sv.top(), sv.bottom());
        sat_ = clamp01(static_cast<float>(x - sv.left()) / (sv.width() - 1));
        val_ = clamp01(1.0f - static_cast<float>(y - sv.top()) / (sv.height() - 1));
    } else if (drag_ == Region::Hue && hue.width() > 1) {
        const int x = std::clamp(pos.x(), hue.left(), hue.right());
        hue_ = clamp01(static_cast<float>(x - hue.left()) / (hue.width() - 1));
        svImage_ = QImage();  // hue changed: the SV square must be regenerated
    } else {
        return;
    }

    syncSwatch();
    update();
    emit colorChanged(color());
}

void ColorPanel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    // Pin the gesture to whichever region it began in, so dragging out of bounds keeps
    // editing that region rather than hopping between square and strip.
    const QPoint pos = e->pos();
    if (svRect().contains(pos)) {
        drag_ = Region::SatVal;
    } else if (hueRect().contains(pos)) {
        drag_ = Region::Hue;
    } else {
        drag_ = Region::None;
        QWidget::mousePressEvent(e);
        return;
    }
    applyPointer(pos);
}

void ColorPanel::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Region::None) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    applyPointer(e->pos());
}

void ColorPanel::syncSwatch() {
    const QColor c = color();
    swatch_->setStyleSheet(QStringLiteral("background:%1;").arg(hexOf(c)));
    hex_->setText(hexOf(c));
}

}  // namespace pe::app
