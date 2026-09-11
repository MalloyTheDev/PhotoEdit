#pragma once

#include "pe/core/Commands.hpp"
#include "pe/core/Geometry.hpp"

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QSpinBox;
class QToolButton;

namespace pe::app {

// Image > Canvas Size: the new canvas dimensions, and where the existing content sits inside
// them.
//
// Canvas Size is not Image Size. Nothing is resampled: the content keeps every pixel it has and
// the canvas rectangle moves around it, so growing adds empty space and shrinking pushes content
// off the edge without destroying it. The anchor grid is the whole of the second half of that
// sentence, which is why it is a 3x3 of buttons rather than a combo box: the shape of the
// control IS the explanation.
class CanvasSizeDialog : public QDialog {
    Q_OBJECT

public:
    CanvasSizeDialog(QWidget* parent, pe::Size current);

    [[nodiscard]] pe::Size size() const;
    [[nodiscard]] pe::CanvasAnchor anchor() const noexcept { return anchor_; }

    // For the tests, and for any future caller that wants to seed the dialog.
    void setSize(pe::Size s);
    void setAnchor(pe::CanvasAnchor a);

private:
    void syncAnchorButtons();
    void syncSummary();

    pe::Size current_{};
    pe::CanvasAnchor anchor_ = pe::CanvasAnchor::Center;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QLabel* summary_ = nullptr;
    QToolButton* anchors_[9]{};
};

}  // namespace pe::app
