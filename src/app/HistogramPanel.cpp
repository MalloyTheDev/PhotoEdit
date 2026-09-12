#include "HistogramPanel.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pe::app {

namespace {
// A 256-bin histogram is exact from a modest sample; half a megapixel keeps the pull cheap and
// well under the composite cap, and the shape is the same as at full resolution.
constexpr int kHistogramSamplePixels = 500'000;

// The bins whose statistics the readout summarises for a given view mode.
const pe::Histogram::Bins& statsBins(const pe::Histogram& h, int mode) {
    switch (mode) {
        case 2:
            return h.red;
        case 3:
            return h.green;
        case 4:
            return h.blue;
        default:
            return h.luma;  // Luminosity (0) and RGB (1)
    }
}
}  // namespace

HistogramView::HistogramView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("HistogramView"));
    setMinimumHeight(120);
}

void HistogramView::setData(const pe::Histogram& hist, int mode) {
    hist_ = hist;
    mode_ = mode;
    update();
}

void HistogramView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // A fixed dark plot ground, the histogram convention, and what the additive RGB overlay needs
    // to read (channels sum toward white); the surrounding panel still follows the app theme.
    p.fillRect(rect(), QColor(26, 26, 30));
    const QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    if (hist_.count == 0 || r.width() <= 0.0 || r.height() <= 0.0) return;

    struct Ch {
        const pe::Histogram::Bins* bins;
        QColor color;
    };
    std::vector<Ch> chans;
    switch (mode_) {
        case 2:
            chans = {{&hist_.red, QColor(220, 90, 90)}};
            break;
        case 3:
            chans = {{&hist_.green, QColor(90, 200, 110)}};
            break;
        case 4:
            chans = {{&hist_.blue, QColor(100, 130, 235)}};
            break;
        case 1:
            chans = {{&hist_.red, QColor(220, 90, 90)},
                     {&hist_.green, QColor(90, 200, 110)},
                     {&hist_.blue, QColor(100, 130, 235)}};
            break;
        case 0:
        default:
            chans = {{&hist_.luma, QColor(190, 190, 195)}};
            break;
    }

    std::uint64_t maxBin = 1;
    for (const Ch& c : chans) {
        for (std::uint64_t v : *c.bins) maxBin = std::max(maxBin, v);
    }
    const double bw = r.width() / static_cast<double>(pe::Histogram::kBins);
    p.setPen(Qt::NoPen);
    for (const Ch& c : chans) {
        // Additive overlay for RGB, so overlapping channels read as brighter (the classic look).
        if (mode_ == 1) p.setCompositionMode(QPainter::CompositionMode_Plus);
        QColor fill = c.color;
        if (mode_ == 1) fill.setAlpha(205);
        p.setBrush(fill);
        for (int i = 0; i < pe::Histogram::kBins; ++i) {
            const double h = static_cast<double>((*c.bins)[static_cast<std::size_t>(i)]) /
                             static_cast<double>(maxBin) * r.height();
            if (h <= 0.0) continue;
            p.drawRect(QRectF(r.left() + i * bw, r.bottom() - h, std::max(1.0, bw), h));
        }
    }
}

HistogramPanel::HistogramPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("HistogramPanel"));
    auto* root = new QVBoxLayout(this);

    channel_ = new QComboBox(this);
    channel_->setObjectName(QStringLiteral("HistogramChannel"));
    channel_->addItem(QStringLiteral("Luminosity"));
    channel_->addItem(QStringLiteral("RGB"));
    channel_->addItem(QStringLiteral("Red"));
    channel_->addItem(QStringLiteral("Green"));
    channel_->addItem(QStringLiteral("Blue"));
    root->addWidget(channel_);

    view_ = new HistogramView(this);
    root->addWidget(view_, 1);

    stats_ = new QLabel(this);
    stats_->setObjectName(QStringLiteral("HistogramStats"));
    stats_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(stats_);

    connect(channel_, &QComboBox::currentIndexChanged, this, [this](int mode) {
        if (view_ != nullptr) view_->setData(hist_, mode);
        refreshStats();
    });

    setEnabled(false);
    refreshStats();
}

HistogramPanel::~HistogramPanel() {
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void HistogramPanel::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    setEnabled(doc_ != nullptr);
    // Compute once on open regardless of visibility, so the dock is right the moment it is raised;
    // the per-change recompute below is what stays visibility-gated.
    refresh();
}

void HistogramPanel::setPreviewSource(std::function<pe::PixelBuffer(int)> source) {
    previewSource_ = std::move(source);
    refresh();
}

void HistogramPanel::onDocumentChanged(const pe::Document&, const pe::DocumentChange& change) {
    switch (change.kind) {
        case pe::DocumentChange::Kind::Selection:
        case pe::DocumentChange::Kind::ActiveLayer:
        case pe::DocumentChange::Kind::DirtyState:
            return;  // none of these change a pixel, so the histogram still holds
        default:
            break;
    }
    stale_ = true;
    if (isVisible()) refresh();
}

void HistogramPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (stale_) refresh();
}

void HistogramPanel::refresh() {
    if (doc_ != nullptr && previewSource_) {
        const pe::PixelBuffer img = previewSource_(kHistogramSamplePixels);
        hist_ = img.isEmpty() ? pe::Histogram{} : pe::computeHistogram(img);
    } else {
        hist_ = pe::Histogram{};
    }
    if (view_ != nullptr) {
        view_->setData(hist_, channel_ != nullptr ? channel_->currentIndex() : 0);
    }
    refreshStats();
    stale_ = false;
}

void HistogramPanel::refreshStats() {
    if (stats_ == nullptr) return;
    if (hist_.count == 0) {
        stats_->setText(QStringLiteral("No pixels to measure."));
        return;
    }
    const int mode = channel_ != nullptr ? channel_->currentIndex() : 0;
    const pe::ChannelStats s = pe::channelStats(statsBins(hist_, mode));
    stats_->setText(QStringLiteral("Mean %1   Median %2   Std dev %3   Pixels %4")
                        .arg(s.mean, 0, 'f', 1)
                        .arg(s.median)
                        .arg(s.stdDev, 0, 'f', 1)
                        .arg(static_cast<qulonglong>(hist_.count)));
}

}  // namespace pe::app
