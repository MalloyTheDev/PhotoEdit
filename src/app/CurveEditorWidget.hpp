#pragma once

#include <QWidget>

#include <cstddef>
#include <utility>
#include <vector>

namespace pe::app {

// An interactive tone-curve editor: a square plot of the identity-anchored curve with draggable
// control points. Points are (input, output) in [0,1], kept sorted by input with the first pinned
// at x==0 and the last at x==1 (only their y moves); interior points move freely between their
// neighbours. Click empty space to add a point, drag to move, right-click an interior point to
// delete. Emits pointsChanged() on every edit. The curve is drawn as straight segments between
// points, matching the engine's piecewise-linear pe::Curves::evalCurve so the preview is faithful.
class CurveEditorWidget : public QWidget {
    Q_OBJECT

public:
    explicit CurveEditorWidget(QWidget* parent = nullptr);

    // Replace the curve's points (sanitized: clamped to [0,1], sorted, deduped, >=2, endpoints
    // forced to x==0 / x==1). Does NOT emit pointsChanged (it reflects an external set).
    void setPoints(std::vector<std::pair<float, float>> pts);
    [[nodiscard]] const std::vector<std::pair<float, float>>& points() const noexcept {
        return points_;
    }
    void resetToIdentity();  // {(0,0),(1,1)} — emits pointsChanged

    [[nodiscard]] QSize sizeHint() const override { return QSize(256, 256); }
    [[nodiscard]] QSize minimumSizeHint() const override { return QSize(160, 160); }

signals:
    void pointsChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    [[nodiscard]] QRectF plotRect() const;  // the inset drawing area (widget space)
    [[nodiscard]] QPointF toWidget(float x, float y) const;          // curve [0,1]^2 -> widget px
    [[nodiscard]] std::pair<float, float> toCurve(QPointF w) const;  // widget px -> curve [0,1]^2
    // Index of the control point within grab radius of widget point `w`, or -1.
    [[nodiscard]] int hitPoint(QPointF w) const;
    void sanitize();  // clamp/sort/dedup/pin endpoints; keeps >=2 points

    std::vector<std::pair<float, float>> points_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    int dragIndex_ = -1;  // control point being dragged, or -1
};

}  // namespace pe::app
