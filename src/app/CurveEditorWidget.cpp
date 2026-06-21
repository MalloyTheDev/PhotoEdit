#include "CurveEditorWidget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace pe::app {

namespace {
constexpr double kPad = 10.0;        // margin around the plot (widget space)
constexpr double kGrabRadius = 9.0;  // px within which a click grabs a control point
constexpr std::size_t kMaxPoints = 32;
[[nodiscard]] float clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}
}  // namespace

CurveEditorWidget::CurveEditorWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 160);
    setMouseTracking(false);
}

void CurveEditorWidget::sanitize() {
    for (auto& [x, y] : points_) {
        x = clamp01(x);
        y = clamp01(y);
    }
    std::stable_sort(points_.begin(), points_.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<float, float>> clean;
    for (const auto& pt : points_) {
        if (clean.empty() || pt.first > clean.back().first) clean.push_back(pt);
    }
    if (clean.size() < 2) clean = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    // Pin the endpoints to the input extremes so the curve always spans [0,1].
    clean.front().first = 0.0f;
    clean.back().first = 1.0f;
    points_ = std::move(clean);
}

void CurveEditorWidget::setPoints(std::vector<std::pair<float, float>> pts) {
    points_ = std::move(pts);
    sanitize();
    update();
}

void CurveEditorWidget::resetToIdentity() {
    points_ = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    dragIndex_ = -1;
    update();
    emit pointsChanged();
}

QRectF CurveEditorWidget::plotRect() const {
    return QRectF(kPad, kPad, std::max(1.0, width() - 2 * kPad),
                  std::max(1.0, height() - 2 * kPad));
}

QPointF CurveEditorWidget::toWidget(float x, float y) const {
    const QRectF r = plotRect();
    return QPointF(r.left() + static_cast<double>(x) * r.width(),
                   r.bottom() - static_cast<double>(y) * r.height());  // y up
}

std::pair<float, float> CurveEditorWidget::toCurve(QPointF w) const {
    const QRectF r = plotRect();
    const float x = static_cast<float>((w.x() - r.left()) / r.width());
    const float y = static_cast<float>((r.bottom() - w.y()) / r.height());
    return {clamp01(x), clamp01(y)};
}

int CurveEditorWidget::hitPoint(QPointF w) const {
    // Nearest control point within the grab radius (so overlapping handles grab the closest one,
    // not just the lowest-index one).
    int best = -1;
    double bestDist = kGrabRadius;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const QPointF p = toWidget(points_[i].first, points_[i].second);
        const double d = std::hypot(p.x() - w.x(), p.y() - w.y());
        if (d <= bestDist) {
            bestDist = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void CurveEditorWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = plotRect();

    p.fillRect(rect(), QColor(40, 44, 52));
    p.fillRect(r, QColor(28, 31, 37));

    // Quarter grid + identity diagonal.
    p.setPen(QPen(QColor(70, 76, 86), 1.0));
    for (int i = 1; i < 4; ++i) {
        const double fx = r.left() + r.width() * i / 4.0;
        const double fy = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(fx, r.top()), QPointF(fx, r.bottom()));
        p.drawLine(QPointF(r.left(), fy), QPointF(r.right(), fy));
    }
    p.setPen(QPen(QColor(70, 76, 86), 1.0, Qt::DashLine));
    p.drawLine(toWidget(0.0f, 0.0f), toWidget(1.0f, 1.0f));
    p.setPen(QPen(QColor(90, 96, 106), 1.0));
    p.drawRect(r);

    // The curve: straight segments between sorted points (matches engine linear interpolation).
    QPainterPath path;
    path.moveTo(toWidget(points_.front().first, points_.front().second));
    for (std::size_t i = 1; i < points_.size(); ++i) {
        path.lineTo(toWidget(points_[i].first, points_[i].second));
    }
    p.setPen(QPen(QColor(225, 228, 234), 2.0));
    p.drawPath(path);

    // Control-point handles (the dragged one accented).
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const QPointF c = toWidget(points_[i].first, points_[i].second);
        const bool active = static_cast<int>(i) == dragIndex_;
        p.setBrush(active ? QColor(95, 170, 250) : QColor(225, 228, 234));
        p.setPen(QPen(QColor(20, 22, 26), 1.0));
        p.drawEllipse(c, 4.0, 4.0);
    }
}

void CurveEditorWidget::mousePressEvent(QMouseEvent* e) {
    const QPointF w = e->position();
    const int hit = hitPoint(w);

    if (e->button() == Qt::RightButton) {
        // Delete an interior point (endpoints are pinned and not removable).
        if (hit > 0 && hit + 1 < static_cast<int>(points_.size())) {
            points_.erase(points_.begin() + hit);
            dragIndex_ = -1;
            update();
            emit pointsChanged();
        }
        return;
    }
    if (e->button() != Qt::LeftButton) return;

    if (hit >= 0) {
        dragIndex_ = hit;  // grab the existing point
    } else if (points_.size() < kMaxPoints) {
        // Add a new interior point at the cursor and start dragging it.
        const auto [cx, cy] = toCurve(w);
        const std::size_t before = points_.size();
        points_.emplace_back(cx, cy);
        sanitize();
        if (points_.size() == before) {
            dragIndex_ = -1;  // collided with an existing x: the add was dropped — a no-op click
            return;           // (don't emit / rebuild the preview for nothing)
        }
        // Re-find the inserted point (sanitize sorts) so the drag tracks it.
        dragIndex_ = hitPoint(w);
        if (dragIndex_ < 0) dragIndex_ = hitPoint(toWidget(cx, cy));
        update();
        emit pointsChanged();
    }
}

void CurveEditorWidget::mouseMoveEvent(QMouseEvent* e) {
    if (dragIndex_ < 0) return;
    const auto [cx, cy] = toCurve(e->position());
    const int last = static_cast<int>(points_.size()) - 1;
    auto& pt = points_[static_cast<std::size_t>(dragIndex_)];
    pt.second = cy;  // y always free
    if (dragIndex_ == 0) {
        pt.first = 0.0f;  // pinned endpoints: x fixed
    } else if (dragIndex_ == last) {
        pt.first = 1.0f;
    } else {
        // Keep interior x strictly between its neighbours so the sort order (and the drag index)
        // stays stable mid-drag. Neighbours are always distinct (sanitize dedups), so the midpoint
        // is strictly between them — use it when the gap is too small for the 1e-3 margins, so x
        // never collapses to/past a neighbour (which would break strictly-increasing ordering).
        const float a = points_[static_cast<std::size_t>(dragIndex_ - 1)].first;
        const float b = points_[static_cast<std::size_t>(dragIndex_ + 1)].first;
        const float lo = a + 1e-3f;
        const float hi = b - 1e-3f;
        pt.first = (lo <= hi) ? std::clamp(cx, lo, hi) : 0.5f * (a + b);
    }
    update();
    emit pointsChanged();
}

void CurveEditorWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) dragIndex_ = -1;
}

}  // namespace pe::app
