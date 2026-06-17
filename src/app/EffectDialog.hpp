#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QSlider;

namespace pe {
class Document;
class PaintCommand;
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
        QString label;
        double min = 0.0;
        double max = 1.0;
        double value = 0.0;
        int decimals = 2;  // 0 => integer parameter
    };

    // Builds a PaintCommand from the current parameter values, or nullptr if the effect
    // cannot apply (no paintable active layer, empty content, over budget).
    using CommandFactory =
        std::function<std::unique_ptr<pe::PaintCommand>(const std::vector<double>&)>;

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
    std::unique_ptr<pe::PaintCommand> preview_;  // provisional, applied-to-doc command

    std::vector<QDoubleSpinBox*> spins_;
    std::vector<QSlider*> sliders_;
    QCheckBox* previewChk_ = nullptr;
    bool syncing_ = false;  // guard: programmatic slider<->spin sync must not re-enter
};

}  // namespace pe::app
