#include "NewDocumentDialog.hpp"

#include "DimensionSpin.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QVBoxLayout>

#include <array>

namespace pe::app {

namespace {

struct Preset {
    const char* name;
    int width;
    int height;
    int ppi;
};

// A short, honest set: the sizes someone actually starts from. Index 0 is Custom and carries no
// dimensions; the rest fill the fields. Print sizes carry 300 ppi because that is the number
// that makes their pixel dimensions mean what the name says.
constexpr std::array<Preset, 7> kPresets = {{
    {"Custom", 0, 0, 0},
    {"1920 x 1080  (1080p)", 1920, 1080, 72},
    {"1280 x 720  (720p)", 1280, 720, 72},
    {"1080 x 1080  (Square)", 1080, 1080, 72},
    {"800 x 600", 800, 600, 72},
    {"A4 @ 300  (2480 x 3508)", 2480, 3508, 300},
    {"Letter @ 300  (2550 x 3300)", 2550, 3300, 300},
}};

}  // namespace

NewDocumentDialog::NewDocumentDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("NewDocumentDialog"));
    setWindowTitle(QStringLiteral("New Document"));

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    preset_ = new QComboBox(this);
    preset_->setObjectName(QStringLiteral("NewPreset"));
    for (const Preset& p : kPresets) preset_->addItem(QString::fromUtf8(p.name));
    form->addRow(QStringLiteral("Preset"), preset_);

    // 800x600 is where File > New used to land unconditionally, so it stays the default: the
    // change is that it is now a starting point rather than the only point.
    width_ = makeDimensionSpinBox(this, 800);
    width_->setObjectName(QStringLiteral("NewWidth"));
    height_ = makeDimensionSpinBox(this, 600);
    height_->setObjectName(QStringLiteral("NewHeight"));
    form->addRow(QStringLiteral("Width"), width_);
    form->addRow(QStringLiteral("Height"), height_);

    resolution_ = new QSpinBox(this);
    resolution_->setObjectName(QStringLiteral("NewResolution"));
    resolution_->setRange(1, 10000);
    resolution_->setValue(72);
    resolution_->setSuffix(QStringLiteral(" ppi"));
    form->addRow(QStringLiteral("Resolution"), resolution_);

    depth_ = new QComboBox(this);
    depth_->setObjectName(QStringLiteral("NewDepth"));
    // The stored value is the enum, not the row, so reordering the list cannot silently change
    // which depth a selection means.
    depth_->addItem(QStringLiteral("8 Bits/Channel"), static_cast<int>(pe::BitDepth::U8));
    depth_->addItem(QStringLiteral("16 Bits/Channel"), static_cast<int>(pe::BitDepth::U16));
    depth_->addItem(QStringLiteral("32 Bits/Channel"), static_cast<int>(pe::BitDepth::F32));
    form->addRow(QStringLiteral("Depth"), depth_);

    background_ = new QComboBox(this);
    background_->setObjectName(QStringLiteral("NewBackground"));
    background_->addItem(QStringLiteral("White"), static_cast<int>(Background::White));
    background_->addItem(QStringLiteral("Transparent"), static_cast<int>(Background::Transparent));
    form->addRow(QStringLiteral("Background"), background_);

    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int i) { applyPreset(i); });
    connect(width_, &QSpinBox::valueChanged, this, [this] { markCustom(); });
    connect(height_, &QSpinBox::valueChanged, this, [this] { markCustom(); });
    connect(resolution_, &QSpinBox::valueChanged, this, [this] { markCustom(); });
}

void NewDocumentDialog::applyPreset(int index) {
    if (index <= 0 || index >= static_cast<int>(kPresets.size())) return;  // Custom or out of range
    const Preset& p = kPresets[static_cast<std::size_t>(index)];
    applyingPreset_ = true;  // these writes must not bounce the combo back to Custom
    width_->setValue(p.width);
    height_->setValue(p.height);
    resolution_->setValue(p.ppi);
    applyingPreset_ = false;
}

void NewDocumentDialog::markCustom() {
    // A field the user changed no longer matches the named preset, so stop claiming it does.
    // Guarded so the preset filling the fields does not immediately un-name itself.
    if (applyingPreset_) return;
    if (preset_ != nullptr) preset_->setCurrentIndex(0);
}

pe::Size NewDocumentDialog::size() const {
    return pe::Size{width_->value(), height_->value()};
}

int NewDocumentDialog::resolutionPpi() const {
    return resolution_->value();
}

pe::BitDepth NewDocumentDialog::depth() const {
    return static_cast<pe::BitDepth>(depth_->currentData().toInt());
}

NewDocumentDialog::Background NewDocumentDialog::background() const {
    return static_cast<Background>(background_->currentData().toInt());
}

}  // namespace pe::app
