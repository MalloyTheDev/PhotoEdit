#include "CurvesDialog.hpp"

#include "CurveEditorWidget.hpp"

#include "pe/core/Command.hpp"
#include "pe/core/Document.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace pe::app {

CurvesDialog::CurvesDialog(QWidget* parent, std::vector<std::pair<float, float>> initialPoints,
                           CommandFactory factory, pe::Document* doc,
                           std::function<void()> onPreview)
    : QDialog(parent), factory_(std::move(factory)), doc_(doc), onPreview_(std::move(onPreview)) {
    setWindowTitle(QStringLiteral("Curves"));
    setModal(true);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("Drag to bend the tone curve. Click to add a point; "
                                              "right-click a point to remove it."),
                               this));

    editor_ = new CurveEditorWidget(this);
    editor_->setPoints(std::move(initialPoints));
    root->addWidget(editor_, 1);
    connect(editor_, &CurveEditorWidget::pointsChanged, this, &CurvesDialog::rebuildPreview);

    auto* bottom = new QHBoxLayout();
    previewChk_ = new QCheckBox(QStringLiteral("Preview"), this);
    previewChk_->setChecked(true);
    connect(previewChk_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            rebuildPreview();
        } else {
            revertPreview();
        }
    });
    auto* resetBtn = new QPushButton(QStringLiteral("Reset"), this);
    connect(resetBtn, &QPushButton::clicked, editor_, &CurveEditorWidget::resetToIdentity);
    bottom->addWidget(previewChk_);
    bottom->addStretch(1);
    bottom->addWidget(resetBtn);
    root->addLayout(bottom);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &CurvesDialog::commit);
    connect(buttons, &QDialogButtonBox::rejected, this, &CurvesDialog::reject);
    root->addWidget(buttons);

    rebuildPreview();  // show the curve at its initial points immediately
}

CurvesDialog::~CurvesDialog() {
    // Defensive: revert any provisional edit if the dialog is torn down with a preview applied.
    if (preview_ && doc_ != nullptr) preview_->undo(*doc_);
}

void CurvesDialog::rebuildPreview() {
    if (doc_ == nullptr || !previewChk_->isChecked()) return;
    // Revert the prior preview so the fresh command captures the original (pre-curve) tiles, then
    // apply it — exactly EffectDialog's dance, so the committed command is one byte-exact undo
    // step.
    if (preview_) {
        preview_->undo(*doc_);
        preview_.reset();
    }
    preview_ = factory_(editor_->points());
    if (preview_) preview_->execute(*doc_);
    if (onPreview_) onPreview_();
}

void CurvesDialog::revertPreview() {
    if (preview_ && doc_ != nullptr) {
        preview_->undo(*doc_);
        preview_.reset();
        if (onPreview_) onPreview_();
    } else {
        preview_.reset();
    }
}

void CurvesDialog::commit() {
    if (doc_ != nullptr) {
        if (preview_) {
            preview_->undo(*doc_);
            preview_.reset();
        }
        std::unique_ptr<pe::Command> cmd = factory_(editor_->points());
        if (cmd) {
            doc_->history().push(std::move(cmd));
        } else if (onPreview_) {
            onPreview_();
        }
    }
    accept();
}

void CurvesDialog::reject() {
    revertPreview();
    QDialog::reject();
}

}  // namespace pe::app
