#include "TextDialog.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pe::app {

namespace {
constexpr int kDefaultSizePx = 48;
}

TextDialog::TextDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Add Text"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    textEdit_ = new QLineEdit(this);
    textEdit_->setPlaceholderText(QStringLiteral("Text"));
    // Bound the input so the rasterized width can't overflow the size math (renderText also caps
    // the rendered area, but limiting at the source keeps a pasted megastring well-behaved).
    textEdit_->setMaxLength(512);
    form->addRow(QStringLiteral("Text:"), textEdit_);

    fontCombo_ = new QFontComboBox(this);
    form->addRow(QStringLiteral("Font:"), fontCombo_);

    sizeSpin_ = new QSpinBox(this);
    sizeSpin_->setRange(4, 2000);
    sizeSpin_->setValue(kDefaultSizePx);
    sizeSpin_->setSuffix(QStringLiteral(" px"));
    form->addRow(QStringLiteral("Size:"), sizeSpin_);

    // Bold / Italic toggles on one row.
    boldCheck_ = new QCheckBox(QStringLiteral("Bold"), this);
    italicCheck_ = new QCheckBox(QStringLiteral("Italic"), this);
    auto* styleRow = new QHBoxLayout;
    styleRow->setContentsMargins(0, 0, 0, 0);
    styleRow->addWidget(boldCheck_);
    styleRow->addWidget(italicCheck_);
    styleRow->addStretch();
    form->addRow(QStringLiteral("Style:"), styleRow);

    // Ink color: a swatch button that opens the color picker.
    colorButton_ = new QPushButton(this);
    connect(colorButton_, &QPushButton::clicked, this, [this] {
        // Opaque only — text ink alpha is always 255 (a transparent glyph is invisible), so the
        // alpha channel is intentionally not offered.
        const QColor picked = QColorDialog::getColor(color_, this, QStringLiteral("Text Color"));
        if (picked.isValid()) {
            color_ = QColor(picked.red(), picked.green(), picked.blue());
            updateColorButton();
        }
    });
    form->addRow(QStringLiteral("Color:"), colorButton_);
    updateColorButton();

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(QStringLiteral("Add"));
    ok->setEnabled(false);  // nothing to add until there is text
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(textEdit_, &QLineEdit::textChanged, this,
            [ok](const QString& s) { ok->setEnabled(!s.isEmpty()); });

    textEdit_->setFocus();
}

QString TextDialog::text() const {
    return textEdit_->text();
}

QFont TextDialog::font() const {
    QFont f = fontCombo_->currentFont();
    f.setPixelSize(sizeSpin_->value());
    f.setBold(boldCheck_->isChecked());
    f.setItalic(italicCheck_->isChecked());
    return f;
}

QColor TextDialog::color() const {
    return color_;
}

void TextDialog::setColor(const QColor& color) {
    color_ = QColor(color.red(), color.green(), color.blue());  // drop any alpha (ink is opaque)
    updateColorButton();
}

void TextDialog::setInitial(const QString& text, const QFont& font, const QColor& color) {
    setWindowTitle(QStringLiteral("Edit Text"));
    textEdit_->setText(text);  // textChanged enables the OK button
    fontCombo_->setCurrentFont(font);
    if (font.pixelSize() > 0) sizeSpin_->setValue(font.pixelSize());
    boldCheck_->setChecked(font.bold());
    italicCheck_->setChecked(font.italic());
    setColor(color);
}

void TextDialog::updateColorButton() {
    // A small filled swatch plus the hex value, so the chosen ink reads clearly regardless of
    // theme.
    QPixmap pm(16, 16);
    pm.fill(color_);
    colorButton_->setIcon(QIcon(pm));
    colorButton_->setText(color_.name(QColor::HexRgb).toUpper());
}

}  // namespace pe::app
