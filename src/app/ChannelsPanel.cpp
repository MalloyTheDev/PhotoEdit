#include "ChannelsPanel.hpp"

#include "Theme.hpp"

#include <QEvent>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QSize>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace pe::app {

namespace {

constexpr int kThumbW = 44;
constexpr int kThumbH = 30;
// Output pixels asked of the preview source. Comfortably more than a thumbnail needs, since
// the renderer's scale factor is a power of two and lands wherever it lands; still small
// enough that a refresh costs nothing next to a repaint.
constexpr int kPreviewPixels = 128 * 128;
constexpr int kEyeColumn = 0;
constexpr int kNameColumn = 1;

// A non-owning QImage over a PixelBuffer, copied so it outlives the buffer.
[[nodiscard]] QImage toImage(const pe::PixelBuffer& buf) {
    if (buf.isEmpty()) return QImage();
    const QImage view(reinterpret_cast<const uchar*>(buf.data()), buf.width(), buf.height(),
                      buf.width() * 4, QImage::Format_RGBA8888);
    return view.copy();
}

// Fit `img` inside the thumbnail box, centred, over `ground`. `checker` draws the
// transparency pattern first, which only the composite row wants: the channel planes are
// opaque grey by construction, and a checkerboard behind them would suggest otherwise.
[[nodiscard]] QIcon thumbnailOf(const QImage& img, QColor ground, bool checker) {
    QPixmap pm(kThumbW, kThumbH);
    pm.fill(ground);
    if (!img.isNull()) {
        QPainter p(&pm);
        const QImage scaled =
            img.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const int x = (kThumbW - scaled.width()) / 2;
        const int y = (kThumbH - scaled.height()) / 2;
        if (checker) {
            constexpr int kCell = 5;
            for (int cy = 0; cy < scaled.height(); cy += kCell) {
                for (int cx = 0; cx < scaled.width(); cx += kCell) {
                    const bool dark = ((cx / kCell) + (cy / kCell)) % 2 == 0;
                    p.fillRect(QRect(x + cx, y + cy, kCell, kCell)
                                   .intersected(QRect(x, y, scaled.width(), scaled.height())),
                               dark ? QColor(90, 90, 90) : QColor(130, 130, 130));
                }
            }
        }
        p.drawImage(x, y, scaled);
    }
    // One pixmap for every state. Qt tints the Selected pixmap with the highlight colour
    // otherwise, which would make a selected row's thumbnail the wrong grey: the whole point
    // of these is that they show the channel's actual values.
    QIcon icon;
    icon.addPixmap(pm, QIcon::Normal);
    icon.addPixmap(pm, QIcon::Selected);
    icon.addPixmap(pm, QIcon::Active);
    return icon;
}

}  // namespace

ChannelsPanel::ChannelsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ChannelsPanel"));
    setAccessibleName(QStringLiteral("Channels"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("ChannelsTree"));
    tree_->setColumnCount(2);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setIconSize(QSize(kThumbW, kThumbH));
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setSectionResizeMode(kEyeColumn, QHeaderView::Fixed);
    tree_->header()->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
    tree_->setColumnWidth(kEyeColumn, 26);
    tree_->installEventFilter(this);
    root->addWidget(tree_, 1);

    // The one action a channel supports today. Photoshop's panel has three more buttons
    // beside it (save a selection as a channel, new, delete), and all three need somewhere on
    // the document to keep a channel, which does not exist yet.
    loadButton_ = new QPushButton(QStringLiteral("Load as Selection"), this);
    loadButton_->setObjectName(QStringLiteral("ChannelsLoadSelection"));
    connect(loadButton_, &QPushButton::clicked, this,
            [this] { emit loadAsSelectionRequested(channelOfCurrentRow()); });
    root->addWidget(loadButton_, 0);

    auto* hint = new QLabel(
        QStringLiteral("Click a channel to view it on its own. The eyes combine. Spot channels, "
                       "and saving a selection as a channel, are not implemented."),
        this);
    hint->setObjectName(QStringLiteral("PanelHint"));
    hint->setWordWrap(true);
    hint->setContentsMargins(8, 6, 8, 6);
    root->addWidget(hint, 0);

    buildRows();
    connect(tree_, &QTreeWidget::itemChanged, this, &ChannelsPanel::onItemChanged);
    connect(tree_, &QTreeWidget::itemClicked, this, &ChannelsPanel::onItemClicked);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { syncLoadButton(); });
    syncLoadButton();
}

ChannelsPanel::~ChannelsPanel() {
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void ChannelsPanel::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    // A new document is a new picture: whatever the last one was being viewed through, the
    // canvas starts on the composite, and the panel must not claim otherwise.
    view_ = pe::ChannelView{};
    syncEyes();
    thumbsStale_ = true;
    if (isVisible()) refreshThumbnails();
    setEnabled(doc_ != nullptr);
    emitView();
}

void ChannelsPanel::setPreviewSource(std::function<pe::PixelBuffer(int)> source) {
    previewSource_ = std::move(source);
    thumbsStale_ = true;
    if (isVisible()) refreshThumbnails();
}

void ChannelsPanel::buildRows() {
    const bool prev = updating_;
    updating_ = true;
    tree_->clear();
    const QString names[kRowCount] = {QStringLiteral("RGB"), QStringLiteral("Red"),
                                      QStringLiteral("Green"), QStringLiteral("Blue")};
    for (int i = 0; i < kRowCount; ++i) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(kNameColumn, names[i]);
        item->setCheckState(kEyeColumn, Qt::Checked);
        item->setData(kNameColumn, Qt::UserRole, i);
        item->setToolTip(kNameColumn,
                         i == Composite
                             ? QStringLiteral("The composite. Click to show every channel.")
                             : QStringLiteral("Click to view the %1 channel on its own, as grey. "
                                              "Use the eye to hide it and leave the others.")
                                   .arg(names[i]));
    }
    tree_->setCurrentItem(tree_->topLevelItem(Composite));
    updating_ = prev;
    syncEyes();
}

void ChannelsPanel::syncEyes() {
    if (tree_ == nullptr) return;
    const bool prev = updating_;
    updating_ = true;  // these writes are ours, not the user's
    const bool on[kRowCount] = {view_.showsAll(), view_.red, view_.green, view_.blue};
    for (int i = 0; i < kRowCount; ++i) {
        if (QTreeWidgetItem* item = tree_->topLevelItem(i)) {
            item->setCheckState(kEyeColumn, on[i] ? Qt::Checked : Qt::Unchecked);
        }
    }
    updating_ = prev;
}

void ChannelsPanel::refreshThumbnails() {
    thumbsStale_ = false;
    if (tree_ == nullptr) return;
    const bool prev = updating_;
    updating_ = true;

    pe::PixelBuffer preview;
    if (doc_ != nullptr && previewSource_) preview = previewSource_(kPreviewPixels);
    const QImage composite = toImage(preview);
    const QColor ground = themeColors(currentTheme()).base;

    const pe::Channel planes[3] = {pe::Channel::Red, pe::Channel::Green, pe::Channel::Blue};
    for (int i = 0; i < kRowCount; ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (item == nullptr) continue;
        if (i == Composite) {
            item->setIcon(kNameColumn, thumbnailOf(composite, ground, true));
        } else {
            // Through the engine's own extractChannel, so a row cannot show one thing while
            // the canvas, which goes through applyChannelView, shows another.
            item->setIcon(
                kNameColumn,
                thumbnailOf(toImage(pe::extractChannel(preview, planes[i - 1])), ground, false));
        }
    }
    updating_ = prev;
}

void ChannelsPanel::onDocumentChanged(const pe::Document&, const pe::DocumentChange& change) {
    switch (change.kind) {
        case pe::DocumentChange::Kind::Selection:
        case pe::DocumentChange::Kind::ActiveLayer:
        case pe::DocumentChange::Kind::DirtyState:
            return;  // none of these can change a pixel, so the thumbnails still hold
        default:
            break;
    }
    thumbsStale_ = true;
    if (isVisible()) refreshThumbnails();
}

void ChannelsPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (thumbsStale_) refreshThumbnails();
}

void ChannelsPanel::setView(pe::ChannelView v) {
    if (v == view_) return;
    view_ = v;
    syncEyes();
}

void ChannelsPanel::emitView() {
    emit viewChanged(view_);
}

void ChannelsPanel::soloRow(int row) {
    const pe::ChannelView before = view_;
    if (row == Composite) {
        view_ = pe::ChannelView{};
    } else {
        view_ = pe::ChannelView{row == Red, row == Green, row == Blue};
    }
    syncEyes();
    if (!(view_ == before)) emitView();
}

void ChannelsPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (updating_ || item == nullptr || column != kEyeColumn || tree_ == nullptr) return;
    const int row = tree_->indexOfTopLevelItem(item);
    const bool on = item->checkState(kEyeColumn) == Qt::Checked;
    const pe::ChannelView before = view_;
    switch (row) {
        case Composite:
            // The composite eye is the whole colour set at once, as it is in Photoshop:
            // turning it off leaves nothing to show, which is black rather than an error.
            view_ = pe::ChannelView{on, on, on};
            break;
        case Red:
            view_.red = on;
            break;
        case Green:
            view_.green = on;
            break;
        case Blue:
            view_.blue = on;
            break;
        default:
            return;
    }
    syncEyes();  // the composite row follows the three below it
    if (!(view_ == before)) emitView();
}

std::optional<pe::Channel> ChannelsPanel::channelOfCurrentRow() const {
    if (tree_ == nullptr) return std::nullopt;
    switch (tree_->indexOfTopLevelItem(tree_->currentItem())) {
        case Red:
            return pe::Channel::Red;
        case Green:
            return pe::Channel::Green;
        case Blue:
            return pe::Channel::Blue;
        default:
            return std::nullopt;  // the composite row: its brightness
    }
}

void ChannelsPanel::syncLoadButton() {
    if (loadButton_ == nullptr) return;
    // Names what it will actually load. The button text stays fixed so the dock does not
    // reflow every time a row is picked, but a button that reads the same for four different
    // outcomes has to say somewhere which one it means.
    const std::optional<pe::Channel> ch = channelOfCurrentRow();
    if (!ch.has_value()) {
        loadButton_->setToolTip(
            QStringLiteral("Load the image's brightness as a selection: the luminosity mask. "
                           "Bright pixels end up selected, dark ones do not, and the greys "
                           "between them are partly selected."));
        return;
    }
    const QString name = ch == pe::Channel::Red     ? QStringLiteral("Red")
                         : ch == pe::Channel::Green ? QStringLiteral("Green")
                                                    : QStringLiteral("Blue");
    loadButton_->setToolTip(
        QStringLiteral("Load the %1 channel as a selection. Bright pixels end up selected, dark "
                       "ones do not, and the greys between them are partly selected.")
            .arg(name));
}

void ChannelsPanel::onItemClicked(QTreeWidgetItem* item, int column) {
    // A click on the eye is the checkbox's business, and arrives separately as itemChanged.
    if (updating_ || item == nullptr || column != kNameColumn || tree_ == nullptr) return;
    soloRow(tree_->indexOfTopLevelItem(item));
}

bool ChannelsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tree_ && event->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            soloRow(tree_->indexOfTopLevelItem(tree_->currentItem()));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace pe::app
