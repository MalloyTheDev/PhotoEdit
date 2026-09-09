#include "AdjustmentsPanel.hpp"

#include "pe/core/Color.hpp"

#include <QAbstractItemView>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QSize>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace pe::app {

namespace {

// The preview strip. Small enough that building one per preset costs nothing (a few
// thousand pixels for the whole panel), wide enough that a tone ramp reads as a ramp.
constexpr int kPreviewW = 64;
constexpr int kPreviewH = 24;
constexpr int kToneRows = 15;  // the rest is the hue sweep; tone is what most presets move

constexpr int kPresetRole = Qt::UserRole;  // the preset a row carries; absent on headings

[[nodiscard]] int to8(float v) {
    return static_cast<int>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

// The reference image as the engine wants it: straight-alpha working-space floats. Opaque
// throughout, because an adjustment leaves fully transparent pixels alone and a transparent
// strip would preview every preset as "no change".
[[nodiscard]] std::vector<pe::Rgbaf> referencePixels() {
    std::vector<pe::Rgbaf> px(static_cast<std::size_t>(kPreviewW) * kPreviewH);
    for (int y = 0; y < kPreviewH; ++y) {
        for (int x = 0; x < kPreviewW; ++x) {
            const float t = static_cast<float>(x) / static_cast<float>(kPreviewW - 1);
            pe::Rgbaf c{};
            if (y < kToneRows) {
                c = pe::Rgbaf{t, t, t, 1.0f};  // tone ramp: Levels, Curves, Exposure move this
            } else {
                // A hue sweep stopping short of wrapping back to red, and deliberately not at
                // full saturation: a preset that REDUCES saturation needs somewhere to move,
                // and at 1.0 Muted would preview as almost no change. Kept to the smaller
                // band so a strip of twenty-six of these reads as a column of previews rather
                // than a column of rainbows.
                const QColor h = QColor::fromHsvF(t * 0.9166f, 0.62f, 0.88f);
                c = pe::Rgbaf{static_cast<float>(h.redF()), static_cast<float>(h.greenF()),
                              static_cast<float>(h.blueF()), 1.0f};
            }
            px[static_cast<std::size_t>(y) * kPreviewW + static_cast<std::size_t>(x)] = c;
        }
    }
    return px;
}

[[nodiscard]] QImage toImage(const std::vector<pe::Rgbaf>& px) {
    QImage img(kPreviewW, kPreviewH, QImage::Format_ARGB32);
    for (int y = 0; y < kPreviewH; ++y) {
        for (int x = 0; x < kPreviewW; ++x) {
            const pe::Rgbaf& c =
                px[static_cast<std::size_t>(y) * kPreviewW + static_cast<std::size_t>(x)];
            img.setPixel(x, y, qRgb(to8(c.r), to8(c.g), to8(c.b)));
        }
    }
    return img;
}

// So the preset table below reads as a table rather than as a page of make_unique
// boilerplate. The adjustments with several knobs have no configuring constructor.
template <class T, class Fn>
[[nodiscard]] std::function<std::unique_ptr<pe::Adjustment>()> configured(Fn setup) {
    return [setup]() -> std::unique_ptr<pe::Adjustment> {
        auto a = std::make_unique<T>();
        setup(*a);
        return a;
    };
}

}  // namespace

AdjustmentsPanel::AdjustmentsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("AdjustmentsPanel"));
    setAccessibleName(QStringLiteral("Adjustments"));

    buildPresets();

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("AdjustmentsList"));
    list_->setIconSize(QSize(kPreviewW, kPreviewH));
    list_->setUniformItemSizes(false);  // heading rows are shorter than preset rows
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->installEventFilter(this);
    v->addWidget(list_, 1);

    // Says where the layer lands. Without it the only way to find out is to add one and go
    // looking, and a panel that drops a layer somewhere unexpected is worse than no panel:
    // the user has to undo something they never saw happen.
    auto* hint = new QLabel(QStringLiteral("Adds an adjustment layer at the top of the stack. "
                                           "Double-click the layer to change its settings."),
                            this);
    hint->setObjectName(QStringLiteral("PanelHint"));
    hint->setWordWrap(true);
    hint->setContentsMargins(8, 6, 8, 6);
    v->addWidget(hint, 0);

    buildRows();

    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item == nullptr) return;
        const QVariant role = item->data(kPresetRole);
        activate(role.isValid() ? role.toInt() : -1);
    });
}

void AdjustmentsPanel::buildPresets() {
    presets_ = {
        // ---- Tone -------------------------------------------------------------------
        {QStringLiteral("Tone"), QStringLiteral("Lighten"), QStringLiteral("Brightness/Contrast"),
         QStringLiteral("Raises brightness across the image, leaving contrast alone."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::BrightnessContrast>(0.22f, 0.0f);
         }},
        {QStringLiteral("Tone"), QStringLiteral("Darken"), QStringLiteral("Brightness/Contrast"),
         QStringLiteral("Lowers brightness across the image, leaving contrast alone."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::BrightnessContrast>(-0.22f, 0.0f);
         }},
        {QStringLiteral("Tone"), QStringLiteral("More Contrast"),
         QStringLiteral("Brightness/Contrast"),
         QStringLiteral("Pushes darks down and lights up, around mid-grey."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::BrightnessContrast>(0.0f, 0.35f);
         }},
        {QStringLiteral("Tone"), QStringLiteral("Less Contrast"),
         QStringLiteral("Brightness/Contrast"),
         QStringLiteral("Pulls the whole range in toward mid-grey."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::BrightnessContrast>(0.0f, -0.35f);
         }},
        {QStringLiteral("Tone"), QStringLiteral("Brighten Midtones"), QStringLiteral("Levels"),
         QStringLiteral("Opens the midtones and leaves black and white where they are."),
         configured<pe::Levels>([](pe::Levels& l) { l.setGamma(1.45f); })},
        {QStringLiteral("Tone"), QStringLiteral("Deepen Midtones"), QStringLiteral("Levels"),
         QStringLiteral("Closes the midtones and leaves black and white where they are."),
         configured<pe::Levels>([](pe::Levels& l) { l.setGamma(0.70f); })},
        {QStringLiteral("Tone"), QStringLiteral("Crush Blacks"), QStringLiteral("Levels"),
         QStringLiteral("Maps the darkest tones to solid black. Shadow detail is lost."),
         configured<pe::Levels>([](pe::Levels& l) { l.setInputBlack(0.12f); })},
        {QStringLiteral("Tone"), QStringLiteral("Lift Shadows"), QStringLiteral("Levels"),
         QStringLiteral("Fades the blacks toward grey, the way a matte print sits."),
         configured<pe::Levels>([](pe::Levels& l) { l.setOutputBlack(0.12f); })},
        {QStringLiteral("Tone"), QStringLiteral("Exposure +1 Stop"), QStringLiteral("Exposure"),
         QStringLiteral("Doubles the light, as opening the lens a stop would."),
         []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Exposure>(1.0f); }},
        {QStringLiteral("Tone"), QStringLiteral("Exposure -1 Stop"), QStringLiteral("Exposure"),
         QStringLiteral("Halves the light, as closing the lens a stop would."),
         []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Exposure>(-1.0f); }},
        {QStringLiteral("Tone"), QStringLiteral("S-Curve"), QStringLiteral("Curves"),
         QStringLiteral("Contrast that holds the ends: steep through the midtones, gentle at "
                        "black and white."),
         configured<pe::Curves>([](pe::Curves& c) {
             c.setPoints({{0.0f, 0.0f}, {0.25f, 0.16f}, {0.75f, 0.84f}, {1.0f, 1.0f}});
         })},
        {QStringLiteral("Tone"), QStringLiteral("Faded Film"), QStringLiteral("Curves"),
         QStringLiteral("Lifted blacks and rolled-off highlights, for a washed print look."),
         configured<pe::Curves>(
             [](pe::Curves& c) { c.setPoints({{0.0f, 0.10f}, {0.5f, 0.52f}, {1.0f, 0.92f}}); })},

        // ---- Colour -----------------------------------------------------------------
        {QStringLiteral("Colour"), QStringLiteral("Vivid"), QStringLiteral("Vibrance"),
         QStringLiteral("Saturates the muted colours and mostly spares the already-strong "
                        "ones."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::Vibrance>(0.55f, 0.05f);
         }},
        {QStringLiteral("Colour"), QStringLiteral("Muted"), QStringLiteral("Vibrance"),
         QStringLiteral("Drains colour without going all the way to grey."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::Vibrance>(-0.45f, -0.15f);
         }},
        {QStringLiteral("Colour"), QStringLiteral("Warmer"), QStringLiteral("Photo Filter"),
         QStringLiteral("An 85 warming filter over the lens, at a third density."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::PhotoFilter>(
                 pe::Rgbaf{236.0f / 255.0f, 138.0f / 255.0f, 0.0f, 1.0f}, 0.35f);
         }},
        {QStringLiteral("Colour"), QStringLiteral("Cooler"), QStringLiteral("Photo Filter"),
         QStringLiteral("An 80 cooling filter over the lens, at a third density."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::PhotoFilter>(pe::Rgbaf{0.0f, 181.0f / 255.0f, 1.0f, 1.0f},
                                                      0.35f);
         }},
        {QStringLiteral("Colour"), QStringLiteral("Cool Shadows, Warm Highlights"),
         QStringLiteral("Color Balance"),
         QStringLiteral("Blue into the shadows and amber into the highlights: the split grade "
                        "films are graded with."),
         configured<pe::ColorBalance>([](pe::ColorBalance& b) {
             b.setShadows(-0.15f, 0.0f, 0.22f);
             b.setHighlights(0.20f, 0.06f, -0.18f);
         })},
        {QStringLiteral("Colour"), QStringLiteral("Sepia"), QStringLiteral("Hue/Saturation"),
         QStringLiteral("Collapses every hue onto one warm brown."),
         configured<pe::HueSaturation>(
             [](pe::HueSaturation& h) { h.setColorize(true, 33.0f, 0.38f); })},
        {QStringLiteral("Colour"), QStringLiteral("Cyanotype"), QStringLiteral("Hue/Saturation"),
         QStringLiteral("Collapses every hue onto the blue of a blueprint."),
         configured<pe::HueSaturation>(
             [](pe::HueSaturation& h) { h.setColorize(true, 205.0f, 0.45f); })},

        // ---- Monochrome -------------------------------------------------------------
        {QStringLiteral("Monochrome"), QStringLiteral("Black & White"),
         QStringLiteral("Black & White"),
         QStringLiteral("Grey, mixed per hue rather than averaged, so colours of the same "
                        "brightness still separate."),
         []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::BlackAndWhite>(); }},
        {QStringLiteral("Monochrome"), QStringLiteral("Black & White, Red Filter"),
         QStringLiteral("Black & White"),
         QStringLiteral("As through a red filter: reds go light, greens and blues go dark. "
                        "Darkens a blue sky."),
         configured<pe::BlackAndWhite>([](pe::BlackAndWhite& b) {
             b.setBand(pe::BlackAndWhite::Reds, 1.70f);
             b.setBand(pe::BlackAndWhite::Yellows, 1.10f);
             b.setBand(pe::BlackAndWhite::Greens, 0.25f);
             b.setBand(pe::BlackAndWhite::Cyans, 0.15f);
             b.setBand(pe::BlackAndWhite::Blues, 0.10f);
             b.setBand(pe::BlackAndWhite::Magentas, 1.30f);
         })},
        {QStringLiteral("Monochrome"), QStringLiteral("Black & White, Blue Filter"),
         QStringLiteral("Black & White"),
         QStringLiteral("As through a blue filter: blues and cyans go light, reds go dark."),
         configured<pe::BlackAndWhite>([](pe::BlackAndWhite& b) {
             b.setBand(pe::BlackAndWhite::Reds, 0.10f);
             b.setBand(pe::BlackAndWhite::Yellows, 0.30f);
             b.setBand(pe::BlackAndWhite::Greens, 0.45f);
             b.setBand(pe::BlackAndWhite::Cyans, 1.25f);
             b.setBand(pe::BlackAndWhite::Blues, 1.70f);
             b.setBand(pe::BlackAndWhite::Magentas, 0.60f);
         })},
        {QStringLiteral("Monochrome"), QStringLiteral("Duotone, Ink and Paper"),
         QStringLiteral("Gradient Map"),
         QStringLiteral("Remaps brightness onto two inks: deep navy in the shadows, warm paper "
                        "in the highlights."),
         []() -> std::unique_ptr<pe::Adjustment> {
             return std::make_unique<pe::GradientMap>(pe::Rgbaf{0.05f, 0.08f, 0.22f, 1.0f},
                                                      pe::Rgbaf{0.98f, 0.95f, 0.86f, 1.0f});
         }},

        // ---- Graphic ----------------------------------------------------------------
        {QStringLiteral("Graphic"), QStringLiteral("Posterize, 4 Levels"),
         QStringLiteral("Posterize"),
         QStringLiteral("Snaps each channel to four steps, for flat bands of colour."),
         []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Posterize>(4); }},
        {QStringLiteral("Graphic"), QStringLiteral("Threshold"), QStringLiteral("Threshold"),
         QStringLiteral("Everything becomes pure black or pure white at the halfway point."),
         []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Threshold>(0.5f); }},
        {QStringLiteral("Graphic"), QStringLiteral("Negative"), QStringLiteral("Invert"),
         QStringLiteral("Inverts every channel, as a colour negative does."),
         []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Invert>(); }},
    };
}

void AdjustmentsPanel::buildRows() {
    if (list_ == nullptr) return;
    list_->clear();
    QString group;
    for (int i = 0; i < presetCount(); ++i) {
        const Preset& p = presets_[i];
        if (p.group != group) {
            group = p.group;
            auto* head = new QListWidgetItem(group, list_);
            QFont f = head->font();
            f.setBold(true);
            head->setFont(f);
            // No flags at all: a heading is not selectable, not clickable, and the arrow
            // keys step straight over it.
            head->setFlags(Qt::NoItemFlags);
        }
        auto* item = new QListWidgetItem(p.name, list_);
        item->setData(kPresetRole, i);
        item->setToolTip(QStringLiteral("%1  (%2)\n%3").arg(p.name, p.type, p.description));
        // The same pixmap for every state. Left to itself Qt tints the Selected pixmap with
        // the highlight colour, which would make a selected row's preview a lie about the
        // colours the preset produces.
        const QPixmap pm = QPixmap::fromImage(preview(i));
        QIcon icon;
        icon.addPixmap(pm, QIcon::Normal);
        icon.addPixmap(pm, QIcon::Selected);
        icon.addPixmap(pm, QIcon::Active);
        item->setIcon(icon);
    }
}

const AdjustmentsPanel::Preset& AdjustmentsPanel::preset(int index) const {
    static const Preset empty{};
    if (index < 0 || index >= presetCount()) return empty;
    return presets_[index];
}

std::unique_ptr<pe::Adjustment> AdjustmentsPanel::makeAdjustment(int index) const {
    const Preset& p = preset(index);
    if (!p.make) return nullptr;
    return p.make();
}

QImage AdjustmentsPanel::referenceStrip() {
    return toImage(referencePixels());
}

QImage AdjustmentsPanel::preview(int index) const {
    const std::unique_ptr<pe::Adjustment> adj = makeAdjustment(index);
    if (adj == nullptr) return QImage();
    std::vector<pe::Rgbaf> px = referencePixels();
    adj->apply(std::span<pe::Rgbaf>(px));
    return toImage(px);
}

int AdjustmentsPanel::presetForRow(int row) const {
    if (list_ == nullptr) return -1;
    const QListWidgetItem* item = list_->item(row);
    if (item == nullptr) return -1;
    const QVariant v = item->data(kPresetRole);
    return v.isValid() ? v.toInt() : -1;
}

void AdjustmentsPanel::activate(int index) {
    if (index < 0 || index >= presetCount()) return;
    emit presetChosen(index);
}

bool AdjustmentsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter || k->key() == Qt::Key_Space) {
            activate(presetForRow(list_->currentRow()));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace pe::app
