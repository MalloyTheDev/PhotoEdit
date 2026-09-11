#include "CanvasSizeDialog.hpp"

#include "DimensionSpin.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>

namespace pe::app {

namespace {

// Row-major, matching pe::CanvasAnchor's own order.
constexpr std::array<const char*, 9> kAnchorNames = {
    "Top left", "Top",         "Top right", "Left",         "Centre",
    "Right",    "Bottom left", "Bottom",    "Bottom right",
};

// Which way the content sits relative to the new space, as an arrow. The grid reads as
// "the content goes here", so the arrows point away from the anchored corner.
constexpr std::array<const char*, 9> kAnchorGlyphs = {
    "↖", "↑", "↗", "←", "•", "→", "↙", "↓", "↘",
};

}  // namespace

CanvasSizeDialog::CanvasSizeDialog(QWidget* parent, pe::Size current)
    : QDialog(parent), current_(current) {
    setObjectName(QStringLiteral("CanvasSizeDialog"));
    setWindowTitle(QStringLiteral("Canvas Size"));

    auto* root = new QVBoxLayout(this);

    auto* currentLabel = new QLabel(
        QStringLiteral("Current: %1 x %2 pixels").arg(current.width).arg(current.height), this);
    currentLabel->setObjectName(QStringLiteral("CanvasSizeCurrent"));
    root->addWidget(currentLabel);

    auto* form = new QFormLayout();
    width_ = makeDimensionSpinBox(this, current.width);
    width_->setObjectName(QStringLiteral("CanvasWidth"));
    height_ = makeDimensionSpinBox(this, current.height);
    height_->setObjectName(QStringLiteral("CanvasHeight"));
    form->addRow(QStringLiteral("Width"), width_);
    form->addRow(QStringLiteral("Height"), height_);
    root->addLayout(form);

    auto* anchorBox = new QGroupBox(QStringLiteral("Anchor"), this);
    auto* grid = new QGridLayout(anchorBox);
    grid->setSpacing(2);
    for (int i = 0; i < 9; ++i) {
        auto* b = new QToolButton(anchorBox);
        b->setObjectName(QStringLiteral("CanvasAnchor%1").arg(i));
        b->setText(QString::fromUtf8(kAnchorGlyphs[static_cast<std::size_t>(i)]));
        b->setToolTip(QString::fromUtf8(kAnchorNames[static_cast<std::size_t>(i)]));
        b->setAccessibleName(QString::fromUtf8(kAnchorNames[static_cast<std::size_t>(i)]));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFixedSize(28, 28);
        connect(b, &QToolButton::clicked, this,
                [this, i] { setAnchor(static_cast<pe::CanvasAnchor>(i)); });
        anchors_[i] = b;
        grid->addWidget(b, i / 3, i % 3);
    }
    root->addWidget(anchorBox);

    // Says what will actually happen, in the terms the user is thinking in: how much is being
    // added or cut, and off which edges. The numbers in the boxes do not say that on their own.
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("CanvasSizeSummary"));
    summary_->setWordWrap(true);
    root->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(width_, &QSpinBox::valueChanged, this, [this] { syncSummary(); });
    connect(height_, &QSpinBox::valueChanged, this, [this] { syncSummary(); });

    syncAnchorButtons();
    syncSummary();
}

pe::Size CanvasSizeDialog::size() const {
    return pe::Size{width_->value(), height_->value()};
}

void CanvasSizeDialog::setSize(pe::Size s) {
    width_->setValue(s.width);
    height_->setValue(s.height);
}

void CanvasSizeDialog::setAnchor(pe::CanvasAnchor a) {
    anchor_ = a;
    syncAnchorButtons();
    syncSummary();
}

void CanvasSizeDialog::syncAnchorButtons() {
    for (int i = 0; i < 9; ++i) {
        if (anchors_[i] != nullptr) {
            anchors_[i]->setChecked(i == static_cast<int>(anchor_));
        }
    }
}

void CanvasSizeDialog::syncSummary() {
    if (summary_ == nullptr) return;
    const pe::Size want = size();
    if (want.width == current_.width && want.height == current_.height) {
        summary_->setText(QStringLiteral("Same as the current size, so nothing would change."));
        return;
    }
    const pe::Point off = pe::canvasAnchorOffset(current_, want, anchor_);
    const int left = off.x;
    const int top = off.y;
    const int right = (want.width - current_.width) - left;
    const int bottom = (want.height - current_.height) - top;
    const auto edge = [](int n, const QString& grew, const QString& cut) {
        return n >= 0 ? QStringLiteral("%1 %2").arg(n).arg(grew)
                      : QStringLiteral("%1 %2").arg(-n).arg(cut);
    };
    summary_->setText(
        QStringLiteral("Left %1, right %2, top %3, bottom %4. Nothing is resampled, and "
                       "anything pushed off the canvas is kept, not deleted.")
            .arg(edge(left, QStringLiteral("added"), QStringLiteral("cut")),
                 edge(right, QStringLiteral("added"), QStringLiteral("cut")),
                 edge(top, QStringLiteral("added"), QStringLiteral("cut")),
                 edge(bottom, QStringLiteral("added"), QStringLiteral("cut"))));
}

}  // namespace pe::app
