#include "PropertiesPanel.hpp"

#include "pe/core/PixelFormat.hpp"

#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace pe::app {

namespace {

constexpr auto kDash = "\xE2\x80\x94";  // em dash, shown for every value when no doc

// Human-readable names for the document's color model and storage depth. These
// mirror the menus a layered editor exposes for Image > Mode.
[[nodiscard]] const char* colorModeName(pe::ColorMode mode) {
    switch (mode) {
        case pe::ColorMode::RGB:
            return "RGB";
        case pe::ColorMode::CMYK:
            return "CMYK";
        case pe::ColorMode::Gray:
            return "Gray";
        case pe::ColorMode::Lab:
            return "Lab";
        case pe::ColorMode::Indexed:
            return "Indexed";
        case pe::ColorMode::Bitmap:
            return "Bitmap";
    }
    return "RGB";
}

[[nodiscard]] const char* bitDepthName(pe::BitDepth depth) {
    switch (depth) {
        case pe::BitDepth::U8:
            return "8 Bits/Channel";
        case pe::BitDepth::U16:
            return "16 Bits/Channel";
        case pe::BitDepth::F32:
            return "32 Bits/Channel";
    }
    return "8 Bits/Channel";
}

// A small, bold, dim section header (object-named so the stylesheet can target it).
[[nodiscard]] QLabel* makeSectionHeader(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SectionHeader"));
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    return label;
}

}  // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // --- Canvas ---
    root->addWidget(makeSectionHeader(QStringLiteral("Canvas"), this));
    auto* canvasForm = new QFormLayout();
    width_ = new QLabel(this);
    height_ = new QLabel(this);
    resolution_ = new QLabel(this);
    mode_ = new QLabel(this);
    depth_ = new QLabel(this);
    canvasForm->addRow(QStringLiteral("Width"), width_);
    canvasForm->addRow(QStringLiteral("Height"), height_);
    canvasForm->addRow(QStringLiteral("Resolution"), resolution_);
    canvasForm->addRow(QStringLiteral("Mode"), mode_);
    canvasForm->addRow(QStringLiteral("Depth"), depth_);
    root->addLayout(canvasForm);

    // --- Rulers & Grids ---
    root->addWidget(makeSectionHeader(QStringLiteral("Rulers & Grids"), this));
    auto* rulersForm = new QFormLayout();
    units_ = new QLabel(this);
    rulersForm->addRow(QStringLiteral("Units"), units_);
    root->addLayout(rulersForm);

    root->addStretch(1);

    refresh();
}

PropertiesPanel::~PropertiesPanel() {
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void PropertiesPanel::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    refresh();
}

void PropertiesPanel::onDocumentChanged(const pe::Document&, const pe::DocumentChange&) {
    // Cheap to recompute, and any change (geometry, mode, even a profile assign)
    // can touch what we display, so just re-read everything.
    refresh();
}

void PropertiesPanel::refresh() {
    if (doc_ == nullptr) {
        for (QLabel* l : {width_, height_, resolution_, mode_, depth_}) {
            l->setText(QString::fromUtf8(kDash));
        }
        units_->setText(QStringLiteral("Pixels"));  // ruler unit is a session preference
        return;
    }

    const pe::Size size = doc_->canvasSize();
    width_->setText(QStringLiteral("%1 px").arg(size.width));
    height_->setText(QStringLiteral("%1 px").arg(size.height));
    resolution_->setText(QStringLiteral("%1 ppi").arg(doc_->resolutionPpi()));
    mode_->setText(QString::fromUtf8(colorModeName(doc_->colorMode())));
    depth_->setText(QString::fromUtf8(bitDepthName(doc_->bitDepth())));
    units_->setText(QStringLiteral("Pixels"));
}

}  // namespace pe::app
