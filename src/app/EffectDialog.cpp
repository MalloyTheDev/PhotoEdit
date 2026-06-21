#include "EffectDialog.hpp"

#include "pe/core/Command.hpp"   // pe::Command (execute/undo/name)
#include "pe/core/Document.hpp"  // pe::Document, history()

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
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
// Paint a color-swatch button with its current color (a contrasting border keeps it visible).
void styleSwatch(QPushButton* b, const QColor& c) {
    b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #222;").arg(c.name()));
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
        if (p.kind == Param::Check) {
            auto* check = new QCheckBox(this);
            check->setChecked(p.value != 0.0);
            connect(check, &QCheckBox::toggled, this, [this](bool) { rebuildPreview(); });
            form->addRow(p.label, check);
            controls_.push_back(Control{.kind = Param::Check, .check = check});
            continue;
        }
        if (p.kind == Param::Color) {
            const std::size_t idx = controls_.size();  // stable index for the click handler
            auto* swatch = new QPushButton(this);
            swatch->setFixedSize(48, 22);
            const QColor init = QColor::fromRgbF(p.r, p.g, p.b);
            styleSwatch(swatch, init);
            connect(swatch, &QPushButton::clicked, this, [this, swatch, idx] {
                const QColor cur =
                    QColor::fromRgbF(controls_[idx].cr, controls_[idx].cg, controls_[idx].cb);
                const QColor picked = QColorDialog::getColor(cur, this, QStringLiteral("Color"));
                if (!picked.isValid()) return;  // user cancelled
                controls_[idx].cr = picked.redF();
                controls_[idx].cg = picked.greenF();
                controls_[idx].cb = picked.blueF();
                styleSwatch(swatch, picked);
                rebuildPreview();
            });
            form->addRow(p.label, swatch);
            controls_.push_back(
                Control{.kind = Param::Color, .swatch = swatch, .cr = p.r, .cg = p.g, .cb = p.b});
            continue;
        }

        // Slider (the default kind): a slider + spinbox kept in lock-step.
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
        controls_.push_back(Control{.kind = Param::Slider, .spin = spin});
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
    // Flatten controls to doubles IN ORDER: Slider/Check push one, Color pushes three (r,g,b). The
    // factory reads them positionally (must match the Param list it was built with).
    std::vector<double> v;
    v.reserve(controls_.size());
    for (const Control& c : controls_) {
        switch (c.kind) {
            case Param::Slider:
                v.push_back(c.spin->value());
                break;
            case Param::Check:
                v.push_back(c.check->isChecked() ? 1.0 : 0.0);
                break;
            case Param::Color:
                v.push_back(c.cr);
                v.push_back(c.cg);
                v.push_back(c.cb);
                break;
        }
    }
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
        std::unique_ptr<pe::Command> cmd = factory_(values());
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
