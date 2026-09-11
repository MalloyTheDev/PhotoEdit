#pragma once

#include "pe/core/Geometry.hpp"
#include "pe/core/PixelFormat.hpp"

#include <QDialog>

class QComboBox;
class QSpinBox;

namespace pe::app {

// File > New: the dimensions, resolution, bit depth and background of a document about to be
// created.
//
// This dialog exists because there was none: File > New hard-coded 800x600, so Canvas Size was
// the only way to obtain a document of any other shape, which is backwards. The fields are the
// honest subset of what createBlank takes. Colour MODE is deliberately absent: the engine
// models CMYK/Gray/Lab but only acts on RGB, and a menu that silently treats a "CMYK" document
// as RGB would be a lie. Bit depth is offered because the engine genuinely stores and preserves
// 8/16/32-bit layers, and New is the one place depth can be chosen since there is no
// convert-depth command yet.
class NewDocumentDialog : public QDialog {
    Q_OBJECT

public:
    // How the single base layer starts out. A transparent document is the engine's native blank
    // (a base layer with no tiles); White fills that layer, which is what most photographs and
    // print work want to start from.
    enum class Background { White, Transparent };

    explicit NewDocumentDialog(QWidget* parent = nullptr);

    [[nodiscard]] pe::Size size() const;
    [[nodiscard]] int resolutionPpi() const;
    [[nodiscard]] pe::BitDepth depth() const;
    [[nodiscard]] Background background() const;

private:
    void applyPreset(int index);  // a named size fills the fields
    void markCustom();            // editing a field means the preset no longer describes it

    QComboBox* preset_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* resolution_ = nullptr;
    QComboBox* depth_ = nullptr;
    QComboBox* background_ = nullptr;
    bool applyingPreset_ = false;  // guard: our own writes into the fields must not reset preset
};

}  // namespace pe::app
