#pragma once

#include "pe/core/Geometry.hpp"

#include <QDialog>

class QCheckBox;
class QLabel;
class QSpinBox;

namespace pe::app {

// Image > Image Size: the new pixel dimensions to resample the whole document to.
//
// Image Size is not Canvas Size. This RESAMPLES: every pixel is scaled to fit the new size, so
// there is no anchor grid (the content fills the new canvas rather than sitting somewhere inside
// it), but there IS a proportion link, because a resample that changes the aspect ratio stretches
// the picture. The link is on by default, so the common case (same shape, different size) cannot
// be got wrong by editing one field and forgetting the other. The summary spells out that pixels
// are scaled, and warns when the result is large enough to be slow to work with.
class ImageSizeDialog : public QDialog {
    Q_OBJECT

public:
    ImageSizeDialog(QWidget* parent, pe::Size current);

    [[nodiscard]] pe::Size size() const;
    [[nodiscard]] bool constrainProportions() const;

    // For the tests, and for any future caller that wants to seed the dialog.
    void setSize(pe::Size s);
    void setConstrainProportions(bool on);

private:
    void widthEdited();
    void heightEdited();
    void syncSummary();

    pe::Size current_{};
    bool updating_ = false;  // guards the proportion link against reentrant valueChanged
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* link_ = nullptr;
    QLabel* summary_ = nullptr;
};

}  // namespace pe::app
