#pragma once

#include <QColor>
#include <QDialog>
#include <QFont>
#include <QString>

class QCheckBox;
class QFontComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace pe::app {

// Modal "Add Text" dialog: a single line of text plus its style — font family, pixel size, bold,
// italic, and ink color. The Type tool shows it after the user clicks the canvas; on
// QDialog::Accepted the caller reads text() + font() + color() and rasterizes them (TextRender)
// into a re-editable text layer. The Add button is disabled until the text is non-empty.
class TextDialog : public QDialog {
    Q_OBJECT

public:
    explicit TextDialog(QWidget* parent = nullptr);

    [[nodiscard]] QString text() const;
    [[nodiscard]] QFont font() const;    // chosen family at the chosen pixel size, with bold/italic
    [[nodiscard]] QColor color() const;  // chosen ink color (opaque)

    // Seed only the ink color (the Add path starts the swatch from the current foreground color).
    void setColor(const QColor& color);
    // Seed every control from an existing string + font + color (the re-edit path: double-clicking
    // a text layer reopens this dialog with its current content and style).
    void setInitial(const QString& text, const QFont& font, const QColor& color);

private:
    void updateColorButton();  // refresh the swatch button's icon + hex label from color_

    QLineEdit* textEdit_ = nullptr;
    QFontComboBox* fontCombo_ = nullptr;
    QSpinBox* sizeSpin_ = nullptr;
    QCheckBox* boldCheck_ = nullptr;
    QCheckBox* italicCheck_ = nullptr;
    QPushButton* colorButton_ = nullptr;
    QColor color_{0, 0, 0};  // opaque black default; text ink is always opaque
};

}  // namespace pe::app
