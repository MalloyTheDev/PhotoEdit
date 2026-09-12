#pragma once

#include "pe/core/ColorProfile.hpp"  // ColorProfileRef, RenderingIntent (lcms2-free header)

#include <QDialog>

class QCheckBox;
class QComboBox;

namespace pe::app {

// Edit > Assign Profile / Convert to Profile: pick a target colour profile, and (for Convert) the
// rendering intent and black-point compensation.
//
// Assign only re-tags the document, so the numbers are reinterpreted under the new profile; Convert
// transforms the pixels so the picture looks the same in the new space, which is why the intent and
// BPC controls appear only in Convert mode. The offered profiles are the engine's five built-in RGB
// working spaces; loading an ICC file from disk is a later step. This dialog is compiled only when
// the engine has lcms2 (see src/app/CMakeLists.txt), because it builds ColorProfile handles.
class ColorProfileDialog : public QDialog {
public:
    ColorProfileDialog(QWidget* parent, const QString& title, const pe::ColorProfileRef& current,
                       bool withConversionOptions);

    [[nodiscard]] pe::ColorProfileRef profile() const;  // the chosen target working space
    [[nodiscard]] pe::RenderingIntent intent() const;   // Convert only; Relative when no options
    [[nodiscard]] bool blackPointCompensation() const;  // Convert only; true when no options

private:
    QComboBox* space_ = nullptr;
    QComboBox* intent_ = nullptr;  // null unless withConversionOptions
    QCheckBox* bpc_ = nullptr;     // null unless withConversionOptions
};

}  // namespace pe::app
