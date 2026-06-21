#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace pe {
class Document;
class Command;
}  // namespace pe

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSlider;

namespace pe::app {

// A modal editor for adjustments whose parameters are organised into GROUPS that share the same set
// of sliders — Channel Mixer (a group per output channel R/G/B, sliders = source R/G/B + constant)
// and Selective Color (a group per color range, sliders = C/M/Y/K). A combo box picks the active
// group; the sliders below edit just that group; an optional checkbox is a global flag (Monochrome
// / Relative). It drives the same live-preview + single-undo-step commit flow as
// EffectDialog/Curves: each edit applies a provisional command (reverting the prior one), OK
// commits one EditAdjustmentCommand, Cancel/close reverts.
class GroupedSlidersDialog : public QDialog {
    Q_OBJECT

public:
    struct SliderSpec {
        QString label;
        double min = -1.0;
        double max = 1.0;
        int decimals = 2;
    };
    // groups: full state [group][slider] (the single source of truth, edited in place). The factory
    // turns it (+ the checkbox flag) into an undoable command, or nullptr if it can't apply.
    using CommandFactory =
        std::function<std::unique_ptr<pe::Command>(const std::vector<std::vector<double>>&, bool)>;

    GroupedSlidersDialog(QWidget* parent, const QString& title, std::vector<QString> groupNames,
                         std::vector<SliderSpec> sliders, std::vector<std::vector<double>> initial,
                         const QString& checkLabel, bool checkInitial, CommandFactory factory,
                         pe::Document* doc, std::function<void()> onPreview);
    ~GroupedSlidersDialog() override;

protected:
    void reject() override;  // revert the live preview before closing

private:
    void loadGroup(int g);  // reflect values_[g] into the sliders (guarded so it doesn't preview)
    void rebuildPreview();  // revert the prior preview, apply a fresh one from values_ + the flag
    void revertPreview();
    void commit();

    std::vector<SliderSpec> specs_;
    std::vector<std::vector<double>> values_;  // [group][slider]; always current
    int curGroup_ = 0;
    bool loading_ = false;  // guard: programmatic slider loads must not echo as edits
    bool syncing_ = false;  // guard: slider<->spin sync

    std::vector<QDoubleSpinBox*> spins_;
    std::vector<QSlider*> sliders_;
    QComboBox* groupCombo_ = nullptr;
    QCheckBox* flagChk_ = nullptr;  // optional global flag (null if none)
    QCheckBox* previewChk_ = nullptr;

    CommandFactory factory_;
    pe::Document* doc_ = nullptr;  // not owned
    std::function<void()> onPreview_;
    std::unique_ptr<pe::Command> preview_;
};

}  // namespace pe::app
