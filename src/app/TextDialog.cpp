#include "TextDialog.hpp"

#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QLineEdit>
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
    form->addRow(QStringLiteral("Text:"), textEdit_);

    fontCombo_ = new QFontComboBox(this);
    form->addRow(QStringLiteral("Font:"), fontCombo_);

    sizeSpin_ = new QSpinBox(this);
    sizeSpin_->setRange(4, 2000);
    sizeSpin_->setValue(kDefaultSizePx);
    sizeSpin_->setSuffix(QStringLiteral(" px"));
    form->addRow(QStringLiteral("Size:"), sizeSpin_);

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
    return f;
}

}  // namespace pe::app
