#include "GradientsPanel.hpp"

#include <QAbstractItemView>
#include <QEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QSize>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pe::app {

namespace {

constexpr int kSwatchW = 96;
constexpr int kSwatchH = 20;

[[nodiscard]] pe::Rgbaf toRgbaf(const QColor& c) {
    return pe::Rgbaf{static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                     static_cast<float>(c.blueF()), static_cast<float>(c.alphaF())};
}

[[nodiscard]] int to8(float v) {
    return static_cast<int>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

// A stop that fades to transparent is the point of half these presets, so the swatch has to
// show transparency rather than compositing it against one flat colour, which would make
// "foreground to transparent" and "foreground to white" look identical on a light panel.
[[nodiscard]] QColor checkerAt(int x, int y) {
    constexpr int kCell = 5;
    return ((x / kCell) + (y / kCell)) % 2 == 0 ? QColor(90, 90, 90) : QColor(130, 130, 130);
}

}  // namespace

GradientsPanel::GradientsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("GradientsPanel"));
    setAccessibleName(QStringLiteral("Gradients"));

    buildPresets();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("GradientsList"));
    list_->setIconSize(QSize(kSwatchW, kSwatchH));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->installEventFilter(this);
    root->addWidget(list_, 1);

    auto* hint = new QLabel(
        QStringLiteral("Choose a ramp, then drag on the canvas with the Gradient tool. Editing "
                       "stops, and saving your own, are not implemented."),
        this);
    hint->setObjectName(QStringLiteral("PanelHint"));
    hint->setWordWrap(true);
    hint->setContentsMargins(8, 6, 8, 6);
    root->addWidget(hint, 0);

    buildRows();

    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item == nullptr || list_ == nullptr) return;
        activate(list_->row(item));
    });
}

void GradientsPanel::buildPresets() {
    using S = pe::GradientStop;
    const auto fixed = [](float pos, int r, int g, int b, int a = 255) {
        return S{pos,
                 pe::Rgbaf{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                           static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f},
                 pe::StopColor::Fixed};
    };

    presets_ = {
        {QStringLiteral("Foreground to Background"),
         QStringLiteral("Follows the two loaded colours. What the Gradient tool drew before "
                        "there was anything to choose."),
         pe::Gradient::foregroundToBackground()},
        {QStringLiteral("Foreground to Transparent"),
         QStringLiteral("The loaded foreground fading out. The one to reach for when a "
                        "gradient is being used to hide part of a layer."),
         pe::Gradient::foregroundToTransparent()},
        {QStringLiteral("Black to White"),
         QStringLiteral("A plain luminance ramp, whatever colours are loaded. Useful as a "
                        "layer mask."),
         pe::Gradient::twoStop(pe::Rgbaf{0.0f, 0.0f, 0.0f, 1.0f},
                               pe::Rgbaf{1.0f, 1.0f, 1.0f, 1.0f})},
        {QStringLiteral("White to Transparent"),
         QStringLiteral("A fading highlight, for glows and soft edges."),
         pe::Gradient({fixed(0.0f, 255, 255, 255), fixed(1.0f, 255, 255, 255, 0)})},
        {QStringLiteral("Sunset"),
         QStringLiteral("Deep violet through orange to a pale sky. Four stops, so the middle "
                        "of it is not a straight fade between the ends."),
         pe::Gradient({fixed(0.0f, 38, 22, 66), fixed(0.42f, 196, 78, 74),
                       fixed(0.72f, 240, 150, 62), fixed(1.0f, 250, 222, 170)})},
        {QStringLiteral("Deep Water"),
         QStringLiteral("Near-black blue up to a shallow green. Cool, and darker than a "
                        "two-stop blue ramp lands."),
         pe::Gradient(
             {fixed(0.0f, 6, 16, 38), fixed(0.55f, 18, 74, 106), fixed(1.0f, 96, 176, 154)})},
        {QStringLiteral("Copper"),
         QStringLiteral("A metal ramp: dark, a bright band off-centre, then dark again. The "
                        "off-centre highlight is what reads as metal."),
         pe::Gradient({fixed(0.0f, 46, 22, 12), fixed(0.35f, 168, 96, 52),
                       fixed(0.52f, 246, 206, 168), fixed(0.72f, 150, 82, 44),
                       fixed(1.0f, 72, 34, 18)})},
        {QStringLiteral("Spectrum"),
         QStringLiteral("A full hue sweep. For mapping colour onto brightness rather than for "
                        "painting with."),
         pe::Gradient({fixed(0.0f, 255, 0, 0), fixed(0.17f, 255, 255, 0), fixed(0.33f, 0, 255, 0),
                       fixed(0.5f, 0, 255, 255), fixed(0.67f, 0, 0, 255), fixed(0.83f, 255, 0, 255),
                       fixed(1.0f, 255, 0, 0)})},
        {QStringLiteral("Fade to Black"),
         QStringLiteral("Transparent to solid black, for darkening one edge of an image."),
         pe::Gradient({fixed(0.0f, 0, 0, 0, 0), fixed(1.0f, 0, 0, 0)})},
    };
}

const GradientsPanel::Preset& GradientsPanel::preset(int index) const {
    static const Preset empty{};
    if (index < 0 || index >= presetCount()) return empty;
    return presets_[index];
}

QImage GradientsPanel::swatch(int index, QSize size) const {
    if (index < 0 || index >= presetCount() || size.width() <= 0 || size.height() <= 0) {
        return QImage();
    }
    const pe::Gradient& g = presets_[index].gradient;
    const pe::Rgbaf fg = toRgbaf(foreground_);
    const pe::Rgbaf bg = toRgbaf(background_);
    QImage img(size, QImage::Format_ARGB32);
    for (int x = 0; x < size.width(); ++x) {
        const float t =
            size.width() > 1 ? static_cast<float>(x) / static_cast<float>(size.width() - 1) : 0.0f;
        const pe::Rgbaf c = g.sample(t, fg, bg);
        const float a = std::clamp(c.a, 0.0f, 1.0f);
        for (int y = 0; y < size.height(); ++y) {
            // Composited over the checkerboard here rather than left transparent: a QIcon is
            // drawn over the row's background, which is one flat colour, and the fade would
            // then be unreadable against it.
            const QColor under = checkerAt(x, y);
            const int r = static_cast<int>(std::lround(to8(c.r) * a + under.red() * (1.0f - a)));
            const int gc = static_cast<int>(std::lround(to8(c.g) * a + under.green() * (1.0f - a)));
            const int b = static_cast<int>(std::lround(to8(c.b) * a + under.blue() * (1.0f - a)));
            img.setPixel(
                x, y, qRgb(std::clamp(r, 0, 255), std::clamp(gc, 0, 255), std::clamp(b, 0, 255)));
        }
    }
    return img;
}

void GradientsPanel::buildRows() {
    if (list_ == nullptr) return;
    list_->clear();
    for (int i = 0; i < presetCount(); ++i) {
        auto* item = new QListWidgetItem(presets_[i].name, list_);
        item->setToolTip(QStringLiteral("%1\n%2").arg(presets_[i].name, presets_[i].description));
        const QPixmap pm = QPixmap::fromImage(swatch(i, QSize(kSwatchW, kSwatchH)));
        // The same pixmap for every state: Qt tints a Selected pixmap with the highlight
        // colour, and a selected row's swatch would then be the wrong ramp.
        QIcon icon;
        icon.addPixmap(pm, QIcon::Normal);
        icon.addPixmap(pm, QIcon::Selected);
        icon.addPixmap(pm, QIcon::Active);
        item->setIcon(icon);
    }
    if (current_ >= 0 && current_ < list_->count()) list_->setCurrentRow(current_);
}

void GradientsPanel::refreshDynamicSwatches() {
    if (list_ == nullptr) return;
    for (int i = 0; i < presetCount(); ++i) {
        // A fixed ramp draws the same whatever is loaded, so rebuilding its swatch would be
        // work for no change on every colour pick, and the picker emits on every drag.
        if (presets_[i].gradient.isFixed()) continue;
        QListWidgetItem* item = list_->item(i);
        if (item == nullptr) continue;
        const QPixmap pm = QPixmap::fromImage(swatch(i, QSize(kSwatchW, kSwatchH)));
        QIcon icon;
        icon.addPixmap(pm, QIcon::Normal);
        icon.addPixmap(pm, QIcon::Selected);
        icon.addPixmap(pm, QIcon::Active);
        item->setIcon(icon);
    }
}

void GradientsPanel::setColors(const QColor& foreground, const QColor& background) {
    if (foreground == foreground_ && background == background_) return;
    foreground_ = foreground;
    background_ = background;
    refreshDynamicSwatches();
}

void GradientsPanel::activate(int index) {
    if (index < 0 || index >= presetCount()) return;
    current_ = index;
    if (list_ != nullptr && list_->currentRow() != index) list_->setCurrentRow(index);
    emit gradientChosen(index);
}

bool GradientsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter || k->key() == Qt::Key_Space) {
            activate(list_->currentRow());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace pe::app
