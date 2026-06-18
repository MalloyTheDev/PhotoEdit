#include "ExportDialog.hpp"

#include "pe/core/Document.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QVariant>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pe::app {

namespace {

struct FormatInfo {
    pe::ImageFormat fmt;
    const char* label;
};

// The raster formats the Export dialog offers, in menu order. Native (.pedoc) is excluded:
// that is the document's own Save path, not a flattened export.
constexpr std::array<FormatInfo, 4> kFormats{{
    {pe::ImageFormat::Png, "PNG"},
    {pe::ImageFormat::Jpeg, "JPEG"},
    {pe::ImageFormat::Tiff, "TIFF"},
    {pe::ImageFormat::WebP, "WebP"},
}};

constexpr int kDefaultJpegQuality = 90;

// Cap on the canvas area for which the live size estimate runs a real (synchronous) encode.
// Above this, encoding the whole canvas on the UI thread would visibly stall, so the estimate
// is skipped — the export itself is unaffected. ~6 MP encodes in well under a frame's budget.
constexpr int64_t kMaxEstimatePixels = 6'000'000;

// Human-readable byte size for the estimate label.
[[nodiscard]] QString humanSize(std::size_t bytes) {
    if (bytes >= 1024u * 1024u) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f',
                                           1);
    }
    if (bytes >= 1024u) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 bytes").arg(bytes);
}

}  // namespace

ExportDialog::ExportDialog(QWidget* parent, const pe::Document& doc, pe::ImageFormat initialFormat)
    : QDialog(parent), doc_(doc) {
    setWindowTitle(QStringLiteral("Export As"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    // Format chooser: only formats whose codec is compiled into this build.
    formatCombo_ = new QComboBox(this);
    int initialIndex = 0;
    for (const FormatInfo& fi : kFormats) {
        if (!pe::formatAvailable(fi.fmt)) continue;
        if (fi.fmt == initialFormat) initialIndex = formatCombo_->count();
        formatCombo_->addItem(QString::fromUtf8(fi.label), QVariant(static_cast<int>(fi.fmt)));
    }
    form->addRow(QStringLiteral("Format:"), formatCombo_);

    // JPEG quality row (slider + spinbox, kept in sync), shown only for JPEG.
    qualityRow_ = new QWidget(this);
    auto* qrow = new QHBoxLayout(qualityRow_);
    qrow->setContentsMargins(0, 0, 0, 0);
    qualitySlider_ = new QSlider(Qt::Horizontal, qualityRow_);
    qualitySlider_->setRange(1, 100);
    qualitySlider_->setValue(kDefaultJpegQuality);
    qualitySpin_ = new QSpinBox(qualityRow_);
    qualitySpin_->setRange(1, 100);
    qualitySpin_->setValue(kDefaultJpegQuality);
    qrow->addWidget(qualitySlider_, 1);
    qrow->addWidget(qualitySpin_);
    form->addRow(QStringLiteral("Quality:"), qualityRow_);

    // Note shown for the lossless formats (which have no adjustable options).
    losslessNote_ = new QLabel(QStringLiteral("Lossless — no adjustable options."), this);
    losslessNote_->setWordWrap(true);
    form->addRow(QString(), losslessNote_);

    sizeLabel_ = new QLabel(this);
    form->addRow(QStringLiteral("Estimated size:"), sizeLabel_);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Export"));
    // No raster codec compiled in -> empty combo -> nothing to export: disable Export so the
    // accept path can never be a silent no-op.
    buttons->button(QDialogButtonBox::Ok)->setEnabled(formatCombo_->count() > 0);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Slider <-> spinbox sync, and a settled-only estimate refresh. The estimate is heavy (it
    // composites + encodes), so it runs only when the value has SETTLED: never on a mid-drag
    // slider tick (isSliderDown()), but on every other route — keyboard, groove page-step, wheel,
    // and spinbox arrows/typing (which reach the slider through the sync below) — and at end of
    // drag (sliderReleased). updateEstimate itself skips large canvases (see its definition).
    connect(qualitySlider_, &QSlider::valueChanged, this, [this](int v) {
        if (!syncing_) {
            syncing_ = true;
            qualitySpin_->setValue(v);
            syncing_ = false;
        }
        if (!qualitySlider_->isSliderDown()) updateEstimate();
    });
    connect(qualitySpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        if (syncing_) return;
        syncing_ = true;
        qualitySlider_->setValue(v);  // drives the slider's valueChanged -> settled estimate
        syncing_ = false;
    });
    connect(qualitySlider_, &QSlider::sliderReleased, this, &ExportDialog::updateEstimate);

    // Establish the initial format with the change signal NOT yet connected, so the single
    // explicit onFormatChanged() below is the only initial composite+encode (no double pass).
    formatCombo_->setCurrentIndex(initialIndex);
    connect(formatCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ExportDialog::onFormatChanged);
    onFormatChanged();  // set initial option visibility + estimate
}

pe::ImageFormat ExportDialog::selectedFormat() const {
    if (formatCombo_->currentIndex() < 0) return pe::ImageFormat::Unknown;
    return static_cast<pe::ImageFormat>(formatCombo_->currentData().toInt());
}

pe::ExportOptions ExportDialog::options() const {
    pe::ExportOptions opts;
    opts.jpegQuality = qualitySlider_->value();
    return opts;
}

void ExportDialog::onFormatChanged() {
    const bool isJpeg = selectedFormat() == pe::ImageFormat::Jpeg;
    qualityRow_->setVisible(isJpeg);
    losslessNote_->setVisible(!isJpeg);
    updateEstimate();
}

void ExportDialog::updateEstimate() {
    // The estimate is a real encode (composite + codec), run synchronously on the UI thread.
    // To keep the dialog responsive, only do it for a moderate-size canvas; above the threshold
    // a full encode could stall for hundreds of ms, so skip it (the export itself still honors
    // the chosen format/options — only the live preview number is omitted).
    const pe::Size cs = doc_.canvasSize();
    const int64_t area = static_cast<int64_t>(cs.width) * static_cast<int64_t>(cs.height);
    if (area <= 0) {
        sizeLabel_->setText(QStringLiteral("—"));
        return;
    }
    if (area > kMaxEstimatePixels) {
        sizeLabel_->setText(QStringLiteral("— (image too large to estimate)"));
        return;
    }
    const std::vector<std::byte> bytes = pe::exportDocument(doc_, selectedFormat(), options());
    sizeLabel_->setText(bytes.empty() ? QStringLiteral("—") : humanSize(bytes.size()));
}

}  // namespace pe::app
