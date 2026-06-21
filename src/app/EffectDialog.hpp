#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;

namespace pe {
class Document;
class Command;
}  // namespace pe

namespace pe::app {

// A generic, data-driven modal for an image adjustment or filter. The caller supplies a
// list of named numeric parameters and a factory that turns the current parameter values
// into an undoable pe::PaintCommand (typically via pe::applyAdjustment / pe::applyFilter,
// which already gate on the active selection). The dialog drives a Photoshop-style live
// preview: as the user drags a slider it applies a provisional command straight to the
// document, reverting the previous one first, so the canvas shows the result in place.
//
// On OK the provisional command is committed as a single undo step; on Cancel (or close,
// or unchecking Preview) it is reverted. The preview bypasses the document's observer, so
// the caller passes an onPreview callback that refreshes the canvas.
class EffectDialog : public QDialog {
    Q_OBJECT

public:
    struct Param {
        // A parameter renders as a slider+spinbox (Slider), an on/off checkbox (Check), or a color
        // swatch that opens a color picker (Color). New kinds were appended after the original
        // slider fields so existing positional initializers ({label,min,max,value,decimals}) still
        // mean a Slider.
        enum Kind { Slider, Check, Color };

        QString label;
        double min = 0.0;
        double max = 1.0;
        double value = 0.0;  // Slider: initial value. Check: initial state (0 or 1).
        int decimals = 2;    // 0 => integer parameter (Slider only)
        Kind kind = Slider;
        double r = 0.0, g = 0.0, b = 0.0;  // Color: initial RGB, each [0,1]
    };

    // Builds an undoable command from the current parameter values, or nullptr if the effect cannot
    // apply (no paintable active layer, empty content, over budget). Each parameter contributes to
    // the value vector IN ORDER by kind: a Slider or Check pushes ONE double (Check is 0.0/1.0); a
    // Color pushes THREE (r, g, b each in [0,1]). The factory reads them positionally. Any Command
    // works: a destructive PaintCommand (Adjustments/Filters) or an EditAdjustmentCommand.
    using CommandFactory = std::function<std::unique_ptr<pe::Command>(const std::vector<double>&)>;

    EffectDialog(QWidget* parent, const QString& title, std::vector<Param> params,
                 CommandFactory factory, pe::Document* doc, std::function<void()> onPreview);
    ~EffectDialog() override;

protected:
    void reject() override;  // revert the live preview before closing

private:
    [[nodiscard]] std::vector<double> values() const;
    void rebuildPreview();  // revert the prior preview, apply a fresh one from current values
    void revertPreview();   // drop the preview, restoring the document
    void commit();          // OK: revert preview, then push it to history as one undo step
    void onPreviewToggled(bool on);

    CommandFactory factory_;
    pe::Document* doc_ = nullptr;  // not owned
    std::function<void()> onPreview_;
    std::unique_ptr<pe::Command> preview_;  // provisional, applied-to-doc command

    // One per Param, in order, so values() collects the right doubles per kind.
    struct Control {
        Param::Kind kind = Param::Slider;
        QDoubleSpinBox* spin = nullptr;       // Slider
        QCheckBox* check = nullptr;           // Check
        QPushButton* swatch = nullptr;        // Color
        double cr = 0.0, cg = 0.0, cb = 0.0;  // Color: current RGB [0,1]
    };
    std::vector<Control> controls_;
    QCheckBox* previewChk_ = nullptr;
    bool syncing_ = false;  // guard: programmatic slider<->spin sync must not re-enter
};

}  // namespace pe::app
