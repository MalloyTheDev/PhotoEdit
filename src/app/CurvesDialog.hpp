#pragma once

#include <QDialog>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace pe {
class Document;
class Command;
}  // namespace pe

class QCheckBox;

namespace pe::app {

class CurveEditorWidget;

// Modal Curves editor: an interactive CurveEditorWidget plus the same Photoshop-style live-preview
// / single-undo-step commit flow as EffectDialog (which is slider-only and can't host a curve). As
// the user edits the curve it applies a provisional command straight to the document (reverting the
// previous one first); OK commits one undo step, Cancel/close reverts. The preview bypasses the
// document's observers, so the caller supplies onPreview to refresh the canvas.
class CurvesDialog : public QDialog {
    Q_OBJECT

public:
    // Builds an undoable command from the current curve points, or nullptr if it can't apply.
    using CommandFactory =
        std::function<std::unique_ptr<pe::Command>(const std::vector<std::pair<float, float>>&)>;

    CurvesDialog(QWidget* parent, std::vector<std::pair<float, float>> initialPoints,
                 CommandFactory factory, pe::Document* doc, std::function<void()> onPreview);
    ~CurvesDialog() override;

protected:
    void reject() override;  // revert the live preview before closing

private:
    void rebuildPreview();  // revert prior preview, apply a fresh one from current points
    void revertPreview();   // drop the preview, restoring the document
    void commit();          // OK: revert preview, push a fresh command as one undo step

    CurveEditorWidget* editor_ = nullptr;
    QCheckBox* previewChk_ = nullptr;
    CommandFactory factory_;
    pe::Document* doc_ = nullptr;  // not owned
    std::function<void()> onPreview_;
    std::unique_ptr<pe::Command> preview_;  // provisional, applied-to-doc command
};

}  // namespace pe::app
