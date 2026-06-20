#pragma once

#include <QDialog>
#include <QFont>
#include <QString>

class QFontComboBox;
class QLineEdit;
class QSpinBox;

namespace pe::app {

// Modal "Add Text" dialog: a single line of text, a font family, and a pixel size. The Type tool
// shows it after the user clicks the canvas; on QDialog::Accepted the caller reads text() + font()
// and rasterizes them (TextRender) for stamping onto the active layer. The color is the current
// foreground color (not chosen here). The Add button is disabled until the text is non-empty.
class TextDialog : public QDialog {
    Q_OBJECT

public:
    explicit TextDialog(QWidget* parent = nullptr);

    [[nodiscard]] QString text() const;
    [[nodiscard]] QFont font() const;  // chosen family at the chosen pixel size

    // Seed the controls from an existing string + font (the re-edit path: double-clicking a text
    // layer reopens this dialog with its current content).
    void setInitial(const QString& text, const QFont& font);

private:
    QLineEdit* textEdit_ = nullptr;
    QFontComboBox* fontCombo_ = nullptr;
    QSpinBox* sizeSpin_ = nullptr;
};

}  // namespace pe::app
