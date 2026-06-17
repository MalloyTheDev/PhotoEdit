#include "LayersPanel.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"

#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>
#include <cstdint>
#include <utility>

namespace pe::app {

namespace {
// LayerId <-> QVariant for list-item payloads.
[[nodiscard]] pe::LayerId idOf(const QListWidgetItem* item) {
    return item == nullptr ? pe::kNoLayer
                           : static_cast<pe::LayerId>(item->data(Qt::UserRole).toULongLong());
}
}  // namespace

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* topRow = new QHBoxLayout();
    blend_ = new QComboBox(this);
    for (int i = 0; i < static_cast<int>(pe::BlendMode::Count); ++i) {
        blend_->addItem(QString::fromUtf8(pe::blendModeName(static_cast<pe::BlendMode>(i))));
    }
    opacity_ = new QSpinBox(this);
    opacity_->setRange(0, 100);
    opacity_->setSuffix(QStringLiteral("%"));
    opacity_->setValue(100);
    topRow->addWidget(blend_, 1);
    topRow->addWidget(opacity_);
    root->addLayout(topRow);

    list_ = new QListWidget(this);
    list_->setIconSize(QSize(26, 26));  // per-layer preview thumbnails
    root->addWidget(list_, 1);

    auto* btnRow = new QHBoxLayout();
    addBtn_ = new QPushButton(QStringLiteral("Add"), this);
    dupBtn_ = new QPushButton(QStringLiteral("Dup"), this);
    delBtn_ = new QPushButton(QStringLiteral("Del"), this);
    upBtn_ = new QPushButton(QStringLiteral("▲"), this);
    downBtn_ = new QPushButton(QStringLiteral("▼"), this);
    for (QPushButton* b : {addBtn_, dupBtn_, delBtn_, upBtn_, downBtn_}) btnRow->addWidget(b);
    root->addLayout(btnRow);

    connect(list_, &QListWidget::currentRowChanged, this, &LayersPanel::onRowChanged);
    connect(list_, &QListWidget::itemChanged, this, &LayersPanel::onItemChanged);
    connect(blend_, &QComboBox::currentIndexChanged, this, &LayersPanel::onBlendChanged);
    connect(opacity_, &QSpinBox::editingFinished, this, &LayersPanel::onOpacityEdited);
    connect(addBtn_, &QPushButton::clicked, this, &LayersPanel::onAdd);
    connect(dupBtn_, &QPushButton::clicked, this, &LayersPanel::onDuplicate);
    connect(delBtn_, &QPushButton::clicked, this, &LayersPanel::onDelete);
    connect(upBtn_, &QPushButton::clicked, this, &LayersPanel::onMoveUp);
    connect(downBtn_, &QPushButton::clicked, this, &LayersPanel::onMoveDown);

    rebuild();
}

LayersPanel::~LayersPanel() {
    if (doc_ != nullptr) doc_->removeObserver(this);
}

void LayersPanel::setDocument(pe::Document* doc) {
    if (doc_ == doc) return;
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = doc;
    if (doc_ != nullptr) doc_->addObserver(this);
    rebuild();
}

void LayersPanel::onDocumentChanged(const pe::Document&, const pe::DocumentChange& change) {
    switch (change.kind) {
        case pe::DocumentChange::Kind::LayerStructure:
        case pe::DocumentChange::Kind::LayerProps:
            rebuild();
            break;
        case pe::DocumentChange::Kind::Pixels:
            // Paint hot path: only the edited layer's thumbnail changed. Refresh just
            // that row's icon (one composite) instead of recompositing every layer.
            updateLayerThumbnail(change.layer);
            break;
        case pe::DocumentChange::Kind::ActiveLayer:
            updating_ = true;
            selectActiveInList();
            updating_ = false;
            syncActiveControls();
            updateButtons();
            break;
        default:
            break;  // Pixels / DirtyState / Profile don't affect the layer list
    }
}

void LayersPanel::push(std::unique_ptr<pe::Command> cmd) {
    if (doc_ != nullptr && cmd != nullptr) doc_->history().push(std::move(cmd));
}

pe::LayerId LayersPanel::selectedLayer() const {
    return idOf(list_->currentItem());
}

QIcon LayersPanel::layerThumbnail(std::size_t engineIndex) const {
    constexpr int kThumb = 26;
    if (doc_ == nullptr) return QIcon();
    const pe::Rect bounds = doc_->canvasBounds();
    const std::int64_t area = static_cast<std::int64_t>(bounds.width) * bounds.height;
    if (area <= 0 || area > 4'000'000) return QIcon();  // bound preview-composite cost
    const auto layers = doc_->topLevelLayers();
    if (engineIndex >= layers.size()) return QIcon();

    // Composite this layer alone over the canvas, then fit it onto a small checker.
    const pe::PixelBuffer buf = pe::compositeToImage(layers.subspan(engineIndex, 1), bounds);
    if (buf.isEmpty()) return QIcon();
    const QImage src(reinterpret_cast<const uchar*>(buf.data()), buf.width(), buf.height(),
                     buf.width() * 4, QImage::Format_RGBA8888);

    QPixmap pm(kThumb, kThumb);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const QColor c0(88, 94, 104);
    const QColor c1(66, 72, 82);
    for (int y = 0; y < kThumb; y += 6) {
        for (int x = 0; x < kThumb; x += 6) {
            p.fillRect(x, y, 6, 6, ((x / 6 + y / 6) & 1) ? c0 : c1);
        }
    }
    const QImage scaled = src.scaled(kThumb, kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p.drawImage((kThumb - scaled.width()) / 2, (kThumb - scaled.height()) / 2, scaled);
    p.setPen(QColor(0, 0, 0, 110));
    p.drawRect(0, 0, kThumb - 1, kThumb - 1);
    p.end();
    return QIcon(pm);
}

void LayersPanel::updateLayerThumbnail(pe::LayerId id) {
    if (doc_ == nullptr || id == pe::kNoLayer) {
        rebuild();
        return;
    }
    const std::size_t idx = doc_->topLevelIndexOf(id);
    if (idx == pe::GroupLayer::npos) {
        rebuild();  // not a top-level row (unknown id, or nested in a group)
        return;
    }
    for (int row = 0; row < list_->count(); ++row) {
        if (idOf(list_->item(row)) == id) {
            // Guard: setIcon emits itemChanged, which must not be read back as an edit.
            updating_ = true;
            list_->item(row)->setIcon(layerThumbnail(idx));
            updating_ = false;
            return;
        }
    }
    rebuild();  // row not found: the list is out of sync, rebuild it
}

void LayersPanel::rebuild() {
    updating_ = true;
    list_->clear();
    if (doc_ != nullptr) {
        const auto layers = doc_->topLevelLayers();  // bottom-to-top
        // Display top layer first: walk the stack from top (highest index) down.
        for (std::size_t i = layers.size(); i-- > 0;) {
            const pe::Layer* l = layers[i].get();
            auto* item = new QListWidgetItem(QString::fromStdString(l->name()), list_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(l->visible() ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, static_cast<qulonglong>(l->id()));
            item->setIcon(layerThumbnail(i));
        }
        selectActiveInList();
    }
    updating_ = false;
    syncActiveControls();
    updateButtons();
}

void LayersPanel::selectActiveInList() {
    if (doc_ == nullptr) return;
    const pe::LayerId active = doc_->activeLayer();
    for (int row = 0; row < list_->count(); ++row) {
        if (idOf(list_->item(row)) == active) {
            list_->setCurrentRow(row);
            return;
        }
    }
}

void LayersPanel::syncActiveControls() {
    updating_ = true;
    const pe::Layer* a = doc_ != nullptr ? doc_->findLayer(doc_->activeLayer()) : nullptr;
    const bool has = a != nullptr;
    blend_->setEnabled(has);
    opacity_->setEnabled(has);
    if (has) {
        blend_->setCurrentIndex(static_cast<int>(a->blendMode()));
        opacity_->setValue(static_cast<int>(std::lround(a->opacity() * 100.0f)));
    }
    updating_ = false;
}

void LayersPanel::updateButtons() {
    const bool hasDoc = doc_ != nullptr;
    const bool hasActive = hasDoc && doc_->findLayer(doc_->activeLayer()) != nullptr;
    const std::size_t n = hasDoc ? doc_->topLevelCount() : 0;
    const std::size_t idx =
        hasActive ? doc_->topLevelIndexOf(doc_->activeLayer()) : pe::GroupLayer::npos;
    addBtn_->setEnabled(hasDoc);
    dupBtn_->setEnabled(hasActive);
    delBtn_->setEnabled(hasActive && n > 1);
    upBtn_->setEnabled(hasActive && idx != pe::GroupLayer::npos && idx + 1 < n);
    downBtn_->setEnabled(hasActive && idx != pe::GroupLayer::npos && idx > 0);
}

void LayersPanel::onRowChanged() {
    if (updating_ || doc_ == nullptr) return;
    const pe::LayerId id = selectedLayer();
    if (id != pe::kNoLayer) doc_->setActiveLayer(id);  // session state (not undoable)
}

void LayersPanel::onItemChanged(QListWidgetItem* item) {
    if (updating_ || doc_ == nullptr || item == nullptr) return;
    const pe::LayerId id = idOf(item);
    const pe::Layer* l = doc_->findLayer(id);
    if (l == nullptr) return;
    const bool want = item->checkState() == Qt::Checked;
    if (l->visible() != want) push(std::make_unique<pe::SetVisibilityCommand>(id, want));
}

void LayersPanel::onBlendChanged(int index) {
    if (updating_ || doc_ == nullptr || index < 0) return;
    const pe::LayerId id = doc_->activeLayer();
    const pe::Layer* l = doc_->findLayer(id);
    const auto mode = static_cast<pe::BlendMode>(index);
    if (l != nullptr && l->blendMode() != mode) {
        push(std::make_unique<pe::SetBlendModeCommand>(id, mode));
    }
}

void LayersPanel::onOpacityEdited() {
    if (updating_ || doc_ == nullptr) return;
    const pe::LayerId id = doc_->activeLayer();
    const pe::Layer* l = doc_->findLayer(id);
    if (l == nullptr) return;
    const float want = static_cast<float>(opacity_->value()) / 100.0f;
    // Compare in the spinbox's integer resolution so re-syncing doesn't re-push.
    if (std::lround(l->opacity() * 100.0f) != opacity_->value()) {
        push(std::make_unique<pe::SetOpacityCommand>(id, want));
    }
}

void LayersPanel::onAdd() {
    if (doc_ == nullptr) return;
    const QString name = QStringLiteral("Layer %1").arg(doc_->topLevelCount() + 1);
    auto layer = std::make_unique<pe::PixelLayer>(name.toStdString(), doc_->bitDepth());
    const pe::LayerId newId = layer->id();
    push(std::make_unique<pe::AddLayerCommand>(std::move(layer), doc_->topLevelCount()));  // on top
    doc_->setActiveLayer(newId);
}

void LayersPanel::onDuplicate() {
    if (doc_ == nullptr) return;
    const pe::LayerId id = doc_->activeLayer();
    if (doc_->findLayer(id) != nullptr) push(std::make_unique<pe::DuplicateLayerCommand>(id));
}

void LayersPanel::onDelete() {
    if (doc_ == nullptr || doc_->topLevelCount() <= 1) return;
    const pe::LayerId id = doc_->activeLayer();
    if (doc_->findLayer(id) != nullptr) push(std::make_unique<pe::RemoveLayerCommand>(id));
}

void LayersPanel::onMoveUp() {
    if (doc_ == nullptr) return;
    const pe::LayerId id = doc_->activeLayer();
    if (doc_->findLayer(id) == nullptr) return;
    const std::size_t idx = doc_->topLevelIndexOf(id);
    // Up in the list = toward the top of the stack = a higher engine index.
    if (idx != pe::GroupLayer::npos && idx + 1 < doc_->topLevelCount()) {
        push(std::make_unique<pe::ReorderLayerCommand>(id, idx + 1));
    }
}

void LayersPanel::onMoveDown() {
    if (doc_ == nullptr) return;
    const pe::LayerId id = doc_->activeLayer();
    if (doc_->findLayer(id) == nullptr) return;
    const std::size_t idx = doc_->topLevelIndexOf(id);
    if (idx != pe::GroupLayer::npos && idx > 0) {
        push(std::make_unique<pe::ReorderLayerCommand>(id, idx - 1));
    }
}

}  // namespace pe::app
