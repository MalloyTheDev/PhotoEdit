#include "ColorProfileDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <array>

namespace pe::app {

namespace {

struct SpaceEntry {
    const char* label;
    pe::BuiltinSpace space;
};
// The engine's five built-in RGB working spaces, everyday one first. Handles are built on demand in
// profile(), so the dialog itself holds no lcms2 state.
constexpr std::array<SpaceEntry, 5> kSpaces = {{
    {"sRGB IEC61966-2.1", pe::BuiltinSpace::sRGB},
    {"Display P3", pe::BuiltinSpace::DisplayP3},
    {"Adobe RGB (1998)", pe::BuiltinSpace::AdobeRGB1998},
    {"ProPhoto RGB", pe::BuiltinSpace::ProPhotoRGB},
    {"sRGB (linear)", pe::BuiltinSpace::sRGBLinear},
}};

struct IntentEntry {
    const char* label;
    pe::RenderingIntent intent;
};
constexpr std::array<IntentEntry, 4> kIntents = {{
    {"Relative Colorimetric", pe::RenderingIntent::RelativeColorimetric},
    {"Perceptual", pe::RenderingIntent::Perceptual},
    {"Saturation", pe::RenderingIntent::Saturation},
    {"Absolute Colorimetric", pe::RenderingIntent::AbsoluteColorimetric},
}};

}  // namespace

ColorProfileDialog::ColorProfileDialog(QWidget* parent, const QString& title,
                                       const pe::ColorProfileRef& current,
                                       bool withConversionOptions)
    : QDialog(parent) {
    setObjectName(QStringLiteral("ColorProfileDialog"));
    setWindowTitle(title);

    auto* root = new QVBoxLayout(this);

    const QString currentText = current && current->valid()
                                    ? QString::fromStdString(current->description())
                                    : QStringLiteral("Untagged (no colour profile)");
    auto* currentLabel = new QLabel(QStringLiteral("Current: %1").arg(currentText), this);
    currentLabel->setObjectName(QStringLiteral("ColorProfileCurrent"));
    currentLabel->setWordWrap(true);
    root->addWidget(currentLabel);

    auto* form = new QFormLayout();
    space_ = new QComboBox(this);
    space_->setObjectName(QStringLiteral("ColorProfileSpace"));
    for (const auto& e : kSpaces) {
        space_->addItem(QString::fromUtf8(e.label), static_cast<int>(e.space));
    }
    form->addRow(QStringLiteral("Profile"), space_);

    if (withConversionOptions) {
        intent_ = new QComboBox(this);
        intent_->setObjectName(QStringLiteral("ColorProfileIntent"));
        for (const auto& e : kIntents) {
            intent_->addItem(QString::fromUtf8(e.label), static_cast<int>(e.intent));
        }
        form->addRow(QStringLiteral("Intent"), intent_);
    }
    root->addLayout(form);

    if (withConversionOptions) {
        bpc_ = new QCheckBox(QStringLiteral("Black point compensation"), this);
        bpc_->setObjectName(QStringLiteral("ColorProfileBpc"));
        bpc_->setChecked(true);
        root->addWidget(bpc_);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

pe::ColorProfileRef ColorProfileDialog::profile() const {
    return pe::ColorProfile::builtin(static_cast<pe::BuiltinSpace>(space_->currentData().toInt()));
}

pe::RenderingIntent ColorProfileDialog::intent() const {
    if (intent_ == nullptr) return pe::RenderingIntent::RelativeColorimetric;
    return static_cast<pe::RenderingIntent>(intent_->currentData().toInt());
}

bool ColorProfileDialog::blackPointCompensation() const {
    return bpc_ == nullptr || bpc_->isChecked();
}

}  // namespace pe::app
