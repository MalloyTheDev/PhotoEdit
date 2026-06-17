#include "EffectDialog.hpp"

#include "pe/core/Brush.hpp"     // pe::PaintCommand
#include "pe/core/Document.hpp"  // pe::Document, history()

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace pe::app {

namespace {
// Map a parameter's real value to the integer slider domain (and back), at the parameter's
// decimal resolution. A 2-decimal param uses steps of 0.01; an integer param steps of 1.
[[nodiscard]] int sliderScale(int decimals) {
    int s = 1;
    for (int i = 0; i < decimals; ++i) s *= 10;
    return s;
}
}  // namespace

EffectDialog::EffectDialog(QWidget* parent, const QString& title, std::vector<Param> params,
                           CommandFactory factory, pe::Document* doc,
                           std::function<void()> onPreview)
    : QDialog(parent), factory_(std::move(factory)), doc_(doc), onPreview_(std::move(onPreview)) {
    setWindowTitle(title);
    setModal(true);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    for (const Param& p : params) {
        const int scale = sliderScale(p.decimals);

        auto* spin = new QDoubleSpinBox(this);
        spin->setDecimals(p.decimals);
        spin->setRange(p.min, p.max);
        spin->setSingleStep(1.0 / static_cast<double>(scale));
        spin->setValue(p.value);

        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(static_cast<int>(std::lround(p.min * scale)),
                         static_cast<int>(std::lround(p.max * scale)));
        slider->setValue(static_cast<int>(std::lround(p.value * scale)));

        // Keep the slider and spinbox in lock-step; either edit triggers a fresh preview.
        connect(slider, &QSlider::valueChanged, this, [this, spin, scale](int v) {
            if (syncing_) return;
            syncing_ = true;
            spin->setValue(static_cast<double>(v) / scale);
            syncing_ = false;
            rebuildPreview();
        });
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, slider, scale](double v) {
            if (syncing_) return;
            syncing_ = true;
            slider->setValue(static_cast<int>(std::lround(v * scale)));
            syncing_ = false;
            rebuildPreview();
        });

        auto* row = new QHBoxLayout();
        row->addWidget(slider, 1);
        row->addWidget(spin);
        form->addRow(p.label, row);

        spins_.push_back(spin);
        sliders_.push_back(slider);
        decimals_.push_back(p.decimals);
    }
    root->addLayout(form);

    previewChk_ = new QCheckBox(QStringLiteral("Preview"), this);
    previewChk_->setChecked(true);
    connect(previewChk_, &QCheckBox::toggled, this, &EffectDialog::onPreviewToggled);
    root->addWidget(previewChk_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &EffectDialog::commit);
    connect(buttons, &QDialogButtonBox::rejected, this, &EffectDialog::reject);
    root->addWidget(buttons);

    rebuildPreview();  // show the effect at the initial parameter values immediately
}

EffectDialog::~EffectDialog() {
    // Normal close paths (OK/Cancel/Esc/window-close) resolve preview_ before exec()
    // returns. If the dialog is torn down with a preview still applied (an exceptional
    // path), revert it so a provisional, un-undoable edit never lingers in the document.
    if (preview_ && doc_ != nullptr) preview_->undo(*doc_);
}

std::vector<double> EffectDialog::values() const {
    std::vector<double> v;
    v.reserve(spins_.size());
    for (const QDoubleSpinBox* s : spins_) v.push_back(s->value());
    return v;
}

void EffectDialog::rebuildPreview() {
    if (doc_ == nullptr || !previewChk_->isChecked()) return;
    // Revert the prior preview so the new command captures the original (pre-effect) tiles,
    // then apply the freshly-built command. Mirrors the brush-tool live-preview dance, so
    // the committed command is always a byte-exact single undo step.
    if (preview_) {
        preview_->undo(*doc_);
        preview_.reset();
    }
    preview_ = factory_(values());
    if (preview_) preview_->execute(*doc_);
    if (onPreview_) onPreview_();
}

void EffectDialog::revertPreview() {
    if (preview_ && doc_ != nullptr) {
        preview_->undo(*doc_);
        preview_.reset();
        if (onPreview_) onPreview_();
    } else {
        preview_.reset();
    }
}

void EffectDialog::onPreviewToggled(bool on) {
    if (on) {
        rebuildPreview();
    } else {
        revertPreview();  // show the untouched document while previewing is off
    }
}

void EffectDialog::commit() {
    if (doc_ != nullptr) {
        // The preview may be stale (preview off) — rebuild from the final values so OK always
        // applies what the controls show, regardless of the Preview checkbox state.
        if (preview_) {
            preview_->undo(*doc_);
            preview_.reset();
        }
        std::unique_ptr<pe::PaintCommand> cmd = factory_(values());
        if (cmd) {
            // History re-executes from the original state and notifies observers (the canvas
            // refreshes itself), collapsing the whole edit into one undo step.
            doc_->history().push(std::move(cmd));
        } else if (onPreview_) {
            onPreview_();  // nothing applied (e.g. empty layer); make sure the canvas is clean
        }
    }
    accept();
}

void EffectDialog::reject() {
    revertPreview();
    QDialog::reject();
}

}  // namespace pe::app
