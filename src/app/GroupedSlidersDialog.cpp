#include "GroupedSlidersDialog.hpp"

#include "pe/core/Command.hpp"
#include "pe/core/Document.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>
#include <cstddef>
#include <utility>

namespace pe::app {

namespace {
[[nodiscard]] int sliderScale(int decimals) {
    int s = 1;
    for (int i = 0; i < decimals; ++i) s *= 10;
    return s;
}
}  // namespace

GroupedSlidersDialog::GroupedSlidersDialog(QWidget* parent, const QString& title,
                                           std::vector<QString> groupNames,
                                           std::vector<SliderSpec> sliders,
                                           std::vector<std::vector<double>> initial,
                                           const QString& checkLabel, bool checkInitial,
                                           CommandFactory factory, pe::Document* doc,
                                           std::function<void()> onPreview)
    : QDialog(parent),
      specs_(std::move(sliders)),
      values_(std::move(initial)),
      factory_(std::move(factory)),
      doc_(doc),
      onPreview_(std::move(onPreview)) {
    setWindowTitle(title);
    setModal(true);

    auto* root = new QVBoxLayout(this);

    groupCombo_ = new QComboBox(this);
    for (const QString& g : groupNames) groupCombo_->addItem(g);
    root->addWidget(groupCombo_);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    for (const SliderSpec& s : specs_) {
        const int scale = sliderScale(s.decimals);
        auto* spin = new QDoubleSpinBox(this);
        spin->setDecimals(s.decimals);
        spin->setRange(s.min, s.max);
        spin->setSingleStep(1.0 / static_cast<double>(scale));

        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(static_cast<int>(std::lround(s.min * scale)),
                         static_cast<int>(std::lround(s.max * scale)));

        const std::size_t i = spins_.size();
        connect(slider, &QSlider::valueChanged, this, [this, spin, scale](int v) {
            if (syncing_) return;
            syncing_ = true;
            spin->setValue(static_cast<double>(v) / scale);
            syncing_ = false;
        });
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, slider, scale, i](double v) {
            if (syncing_) {
                // slider drove the spin: still mirror into the model + preview below.
            } else {
                syncing_ = true;
                slider->setValue(static_cast<int>(std::lround(v * scale)));
                syncing_ = false;
            }
            if (loading_) return;  // programmatic group load, not a user edit
            values_[static_cast<std::size_t>(curGroup_)][i] = v;
            rebuildPreview();
        });

        auto* row = new QHBoxLayout();
        row->addWidget(slider, 1);
        row->addWidget(spin);
        form->addRow(s.label, row);
        spins_.push_back(spin);
        sliders_.push_back(slider);
    }
    root->addLayout(form);

    if (!checkLabel.isEmpty()) {
        flagChk_ = new QCheckBox(checkLabel, this);
        flagChk_->setChecked(checkInitial);
        connect(flagChk_, &QCheckBox::toggled, this, [this](bool) { rebuildPreview(); });
        root->addWidget(flagChk_);
    }

    // Switching the group only changes which slice the sliders show; the model is unchanged, so no
    // preview rebuild is needed — just reload the sliders.
    connect(groupCombo_, &QComboBox::currentIndexChanged, this, [this](int g) { loadGroup(g); });

    previewChk_ = new QCheckBox(QStringLiteral("Preview"), this);
    previewChk_->setChecked(true);
    connect(previewChk_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            rebuildPreview();
        } else {
            revertPreview();
        }
    });
    root->addWidget(previewChk_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &GroupedSlidersDialog::commit);
    connect(buttons, &QDialogButtonBox::rejected, this, &GroupedSlidersDialog::reject);
    root->addWidget(buttons);

    loadGroup(0);      // populate the sliders for the first group
    rebuildPreview();  // show the adjustment at its initial values immediately
}

GroupedSlidersDialog::~GroupedSlidersDialog() {
    if (preview_ && doc_ != nullptr) preview_->undo(*doc_);
}

void GroupedSlidersDialog::loadGroup(int g) {
    if (g < 0 || g >= static_cast<int>(values_.size())) return;
    curGroup_ = g;
    loading_ = true;
    for (std::size_t i = 0; i < spins_.size(); ++i) {
        spins_[i]->setValue(values_[static_cast<std::size_t>(g)][i]);
    }
    loading_ = false;
}

void GroupedSlidersDialog::rebuildPreview() {
    if (doc_ == nullptr || !previewChk_->isChecked()) return;
    if (preview_) {
        preview_->undo(*doc_);
        preview_.reset();
    }
    preview_ = factory_(values_, flagChk_ != nullptr && flagChk_->isChecked());
    if (preview_) preview_->execute(*doc_);
    if (onPreview_) onPreview_();
}

void GroupedSlidersDialog::revertPreview() {
    if (preview_ && doc_ != nullptr) {
        preview_->undo(*doc_);
        preview_.reset();
        if (onPreview_) onPreview_();
    } else {
        preview_.reset();
    }
}

void GroupedSlidersDialog::commit() {
    if (doc_ != nullptr) {
        if (preview_) {
            preview_->undo(*doc_);
            preview_.reset();
        }
        std::unique_ptr<pe::Command> cmd =
            factory_(values_, flagChk_ != nullptr && flagChk_->isChecked());
        if (cmd) {
            doc_->history().push(std::move(cmd));
        } else if (onPreview_) {
            onPreview_();
        }
    }
    accept();
}

void GroupedSlidersDialog::reject() {
    revertPreview();
    QDialog::reject();
}

}  // namespace pe::app
