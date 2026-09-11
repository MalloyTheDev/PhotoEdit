#include "ImageSizeDialog.hpp"

#include "DimensionSpin.hpp"

#include "pe/core/Compositor.hpp"  // kMaxCompositeImagePixels (the "very large" warning threshold)

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace pe::app {

ImageSizeDialog::ImageSizeDialog(QWidget* parent, pe::Size current)
    : QDialog(parent), current_(current) {
    setObjectName(QStringLiteral("ImageSizeDialog"));
    setWindowTitle(QStringLiteral("Image Size"));

    auto* root = new QVBoxLayout(this);

    auto* currentLabel = new QLabel(
        QStringLiteral("Current: %1 x %2 pixels").arg(current.width).arg(current.height), this);
    currentLabel->setObjectName(QStringLiteral("ImageSizeCurrent"));
    root->addWidget(currentLabel);

    auto* form = new QFormLayout();
    width_ = makeDimensionSpinBox(this, current.width);
    width_->setObjectName(QStringLiteral("ImageWidth"));
    height_ = makeDimensionSpinBox(this, current.height);
    height_->setObjectName(QStringLiteral("ImageHeight"));
    form->addRow(QStringLiteral("Width"), width_);
    form->addRow(QStringLiteral("Height"), height_);
    root->addLayout(form);

    // On by default: the common resize keeps the picture's shape, and the link is what makes
    // that the thing you get without having to compute the other field yourself.
    link_ = new QCheckBox(QStringLiteral("Constrain proportions"), this);
    link_->setObjectName(QStringLiteral("ImageSizeConstrain"));
    link_->setChecked(true);
    root->addWidget(link_);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("ImageSizeSummary"));
    summary_->setWordWrap(true);
    root->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(width_, &QSpinBox::valueChanged, this, [this] { widthEdited(); });
    connect(height_, &QSpinBox::valueChanged, this, [this] { heightEdited(); });
    connect(link_, &QCheckBox::toggled, this, [this] { syncSummary(); });

    syncSummary();
}

pe::Size ImageSizeDialog::size() const {
    return pe::Size{width_->value(), height_->value()};
}

bool ImageSizeDialog::constrainProportions() const {
    return link_ != nullptr && link_->isChecked();
}

void ImageSizeDialog::setSize(pe::Size s) {
    updating_ = true;  // set both without the link firing between the two writes
    width_->setValue(s.width);
    height_->setValue(s.height);
    updating_ = false;
    syncSummary();
}

void ImageSizeDialog::setConstrainProportions(bool on) {
    if (link_ != nullptr) link_->setChecked(on);
}

void ImageSizeDialog::widthEdited() {
    // Keep the height in step with the ORIGINAL proportions (not the current field ratio), so the
    // link is reversible: doubling the width then halving it returns the exact starting height.
    if (!updating_ && constrainProportions() && current_.width > 0) {
        updating_ = true;
        const long long h = std::llround(static_cast<double>(width_->value()) *
                                         static_cast<double>(current_.height) /
                                         static_cast<double>(current_.width));
        height_->setValue(static_cast<int>(std::clamp<long long>(h, 1, pe::kMaxCanvasDimension)));
        updating_ = false;
    }
    syncSummary();
}

void ImageSizeDialog::heightEdited() {
    if (!updating_ && constrainProportions() && current_.height > 0) {
        updating_ = true;
        const long long w = std::llround(static_cast<double>(height_->value()) *
                                         static_cast<double>(current_.width) /
                                         static_cast<double>(current_.height));
        width_->setValue(static_cast<int>(std::clamp<long long>(w, 1, pe::kMaxCanvasDimension)));
        updating_ = false;
    }
    syncSummary();
}

void ImageSizeDialog::syncSummary() {
    if (summary_ == nullptr) return;
    const pe::Size want = size();
    if (want.width == current_.width && want.height == current_.height) {
        summary_->setText(QStringLiteral("Same as the current size, so nothing would change."));
        return;
    }
    const double mp =
        static_cast<double>(want.width) * static_cast<double>(want.height) / 1'000'000.0;
    QString text = QStringLiteral(
                       "Resampling to %1 x %2 pixels (%3 megapixels). Every pixel is "
                       "scaled to fit the new size.")
                       .arg(want.width)
                       .arg(want.height)
                       .arg(mp, 0, 'f', 1);
    if (static_cast<long long>(want.width) * static_cast<long long>(want.height) >
        pe::kMaxCompositeImagePixels) {
        text += QStringLiteral(" That is very large and may be slow to work with.");
    }
    summary_->setText(text);
}

}  // namespace pe::app
