#include "LayersPanel.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"

#include <QColor>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>

namespace pe::app {

namespace {
// LayerId <-> QVariant for tree-item payloads (id lives in column 0, UserRole).
[[nodiscard]] pe::LayerId idOf(const QTreeWidgetItem* item) {
    return item == nullptr ? pe::kNoLayer
                           : static_cast<pe::LayerId>(item->data(0, Qt::UserRole).toULongLong());
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

    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(2);           // col 0: visibility + layer thumb + name; col 1: mask thumb
    tree_->setIconSize(QSize(26, 26));  // per-layer preview thumbnails
    tree_->setIndentation(14);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);  // multi-select for Group
    tree_->setExpandsOnDoubleClick(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree_->setColumnWidth(1, 34);  // narrow mask-thumbnail column
    root->addWidget(tree_, 1);

    auto* btnRow = new QHBoxLayout();
    addBtn_ = new QPushButton(QStringLiteral("Add"), this);
    dupBtn_ = new QPushButton(QStringLiteral("Dup"), this);
    delBtn_ = new QPushButton(QStringLiteral("Del"), this);
    groupBtn_ = new QPushButton(QStringLiteral("Grp"), this);
    ungroupBtn_ = new QPushButton(QStringLiteral("Ungrp"), this);
    maskBtn_ = new QPushButton(QStringLiteral("Msk"), this);
    upBtn_ = new QPushButton(QStringLiteral("▲"), this);
    downBtn_ = new QPushButton(QStringLiteral("▼"), this);
    groupBtn_->setToolTip(QStringLiteral("Group selected layers (Ctrl+G)"));
    ungroupBtn_->setToolTip(QStringLiteral("Ungroup the active group (Ctrl+Shift+G)"));
    maskBtn_->setToolTip(
        QStringLiteral("Add a layer mask (from the selection if any). Click a mask thumbnail to "
                       "paint into it with the Brush (black hides, white reveals); Alt-click "
                       "toggles it on/off."));
    for (QPushButton* b :
         {addBtn_, dupBtn_, delBtn_, groupBtn_, ungroupBtn_, maskBtn_, upBtn_, downBtn_}) {
        btnRow->addWidget(b);
    }
    root->addLayout(btnRow);

    connect(tree_, &QTreeWidget::currentItemChanged, this, &LayersPanel::onRowChanged);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &LayersPanel::onSelectionChanged);
    connect(tree_, &QTreeWidget::itemChanged, this, &LayersPanel::onItemChanged);
    connect(tree_, &QTreeWidget::itemClicked, this, &LayersPanel::onItemClicked);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &LayersPanel::onItemDoubleClicked);
    connect(tree_, &QTreeWidget::itemExpanded, this, &LayersPanel::onItemExpanded);
    connect(tree_, &QTreeWidget::itemCollapsed, this, &LayersPanel::onItemCollapsed);
    connect(blend_, &QComboBox::currentIndexChanged, this, &LayersPanel::onBlendChanged);
    connect(opacity_, &QSpinBox::editingFinished, this, &LayersPanel::onOpacityEdited);
    connect(addBtn_, &QPushButton::clicked, this, &LayersPanel::onAdd);
    connect(dupBtn_, &QPushButton::clicked, this, &LayersPanel::onDuplicate);
    connect(delBtn_, &QPushButton::clicked, this, &LayersPanel::onDelete);
    connect(groupBtn_, &QPushButton::clicked, this, &LayersPanel::groupSelected);
    connect(ungroupBtn_, &QPushButton::clicked, this, &LayersPanel::ungroupSelected);
    connect(maskBtn_, &QPushButton::clicked, this, &LayersPanel::addMaskForActive);
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
    collapsed_.clear();          // a different document: forget prior expand/collapse state
    maskTarget_ = pe::kNoLayer;  // the mask-edit target belonged to the old document
    rebuild();
}

void LayersPanel::onDocumentChanged(const pe::Document&, const pe::DocumentChange& change) {
    switch (change.kind) {
        case pe::DocumentChange::Kind::LayerStructure:
        case pe::DocumentChange::Kind::LayerProps:
            // A structural/property change may have removed the targeted mask (or its layer); drop
            // the target (notifying CanvasView) before redrawing so the Brush stops aiming at it.
            if (maskTarget_ != pe::kNoLayer) {
                const pe::Layer* t = doc_->findLayer(maskTarget_);
                if (t == nullptr || t->mask() == nullptr) setMaskTarget(pe::kNoLayer);
            }
            rebuild();
            break;
        case pe::DocumentChange::Kind::Pixels:
            // Paint hot path: only the edited layer's thumbnail changed. Refresh just
            // that row's icon (one composite) instead of recompositing every layer.
            updateLayerThumbnail(change.layer);
            break;
        case pe::DocumentChange::Kind::MaskPixels:
            // Mask-paint hot path: only the masked layer's row changed. Refresh its composited
            // thumbnail (col 0, reflects the mask) and its mask thumbnail (col 1) — O(1) — instead
            // of a full tree rebuild. The focus ring (maskTarget_) is preserved.
            updateLayerThumbnail(change.layer);
            updateMaskThumbnail(change.layer);
            break;
        case pe::DocumentChange::Kind::ActiveLayer:
            // Skip re-selection for an active change we ourselves just pushed from a row
            // click — the tree selection is already correct, and re-selecting would
            // collapse a multi-selection (see syncingActive_).
            if (!syncingActive_) {
                updating_ = true;
                selectActiveInTree();
                updating_ = false;
            }
            syncActiveControls();
            updateButtons();
            // Keep the mask-edit target equal to the active layer. An active change driven from
            // OUTSIDE a row click (New Adjustment Layer / Add / Ungroup / undo, or keyboard nav)
            // arrives here, NOT through onRowChanged (which is suppressed by updating_), so
            // reconcile it directly — otherwise the focus ring and CanvasView's MaskPaint mode go
            // stale on a layer whose mask is no longer the active one.
            if (maskTarget_ != pe::kNoLayer && maskTarget_ != doc_->activeLayer()) {
                setMaskTarget(pe::kNoLayer);
            }
            break;
        default:
            break;  // DirtyState / Profile / Selection don't affect the layer tree
    }
}

void LayersPanel::push(std::unique_ptr<pe::Command> cmd) {
    if (doc_ != nullptr && cmd != nullptr) doc_->history().push(std::move(cmd));
}

pe::LayerId LayersPanel::selectedLayer() const {
    return idOf(tree_->currentItem());
}

std::vector<pe::LayerId> LayersPanel::selectedTopLevelIds() const {
    std::vector<pe::LayerId> ids;
    if (doc_ == nullptr) return ids;
    const QList<QTreeWidgetItem*> items = tree_->selectedItems();
    ids.reserve(static_cast<std::size_t>(items.size()));
    for (const QTreeWidgetItem* item : items) {
        const pe::LayerId id = idOf(item);
        if (id != pe::kNoLayer && doc_->topLevelIndexOf(id) != pe::GroupLayer::npos) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool LayersPanel::selectionAllTopLevel() const {
    if (doc_ == nullptr) return false;
    const QList<QTreeWidgetItem*> items = tree_->selectedItems();
    if (items.isEmpty()) return false;
    for (const QTreeWidgetItem* item : items) {
        if (doc_->topLevelIndexOf(idOf(item)) == pe::GroupLayer::npos) return false;
    }
    return true;
}

QIcon LayersPanel::groupIcon() const {
    constexpr int kThumb = 26;
    QPixmap pm(kThumb, kThumb);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // A simple folder glyph (tab + body) drawn in code, so groups read as folders
    // without a bundled asset and without compositing a potentially huge subtree.
    const QColor body(150, 162, 178);
    const QColor edge(40, 44, 52);
    p.setPen(QPen(edge, 1));
    p.setBrush(body.darker(115));
    p.drawRect(QRectF(3.5, 7.5, 9.0, 3.0));  // back tab
    p.setBrush(body);
    p.drawRoundedRect(QRectF(2.5, 9.5, 21.0, 12.0), 2.0, 2.0);  // folder body
    p.end();
    return QIcon(pm);
}

QIcon LayersPanel::adjustmentIcon() const {
    constexpr int kThumb = 26;
    QPixmap pm(kThumb, kThumb);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // A half-filled circle — the conventional "adjustment layer" glyph. Adjustment layers
    // contribute no pixels, so a composited thumbnail would be blank; this reads as a tonal/color
    // adjustment instead.
    const QColor ring(150, 162, 178);
    const QRectF disc(5.5, 5.5, 15.0, 15.0);
    p.setPen(QPen(ring, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(disc);
    p.setPen(Qt::NoPen);
    p.setBrush(ring);
    p.drawPie(disc, 90 * 16, 180 * 16);  // fill the left half
    p.end();
    return QIcon(pm);
}

QIcon LayersPanel::maskThumbnail(const pe::Mask& mask, bool targeted) const {
    constexpr int kThumb = 26;
    if (doc_ == nullptr) return QIcon();
    const pe::Rect b = doc_->canvasBounds();
    if (b.width <= 0 || b.height <= 0) return QIcon();
    // Sample the mask coverage on a small grid — cheap regardless of canvas size (no
    // per-canvas-pixel loop). evaluate() reflects inverted + density; a disabled mask is marked
    // with a cross below.
    QImage gray(kThumb, kThumb, QImage::Format_RGB888);
    for (int ty = 0; ty < kThumb; ++ty) {
        const int dy = b.y + static_cast<int>((static_cast<std::int64_t>(ty) * b.height) / kThumb);
        for (int tx = 0; tx < kThumb; ++tx) {
            const int dx =
                b.x + static_cast<int>((static_cast<std::int64_t>(tx) * b.width) / kThumb);
            const int v = static_cast<int>(std::lround(mask.evaluate(dx, dy) * 255.0f));
            gray.setPixel(tx, ty, qRgb(v, v, v));
        }
    }
    QPixmap pm = QPixmap::fromImage(gray);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!mask.enabled()) {
        // Dim + a red cross: the compositor currently ignores this mask.
        p.fillRect(pm.rect(), QColor(0, 0, 0, 90));
        p.setPen(QPen(QColor(220, 80, 80), 1.5));
        p.drawLine(3, 3, kThumb - 4, kThumb - 4);
        p.drawLine(kThumb - 4, 3, 3, kThumb - 4);
    }
    p.setPen(QColor(0, 0, 0, 140));
    p.drawRect(0, 0, kThumb - 1, kThumb - 1);
    if (targeted) {
        // Bright focus ring: this mask is the Brush's current paint target (Photoshop's mask
        // focus).
        p.setPen(QPen(QColor(90, 170, 250), 2));
        p.drawRect(1, 1, kThumb - 3, kThumb - 3);
    }
    p.end();
    return QIcon(pm);
}

QIcon LayersPanel::layerThumbnail(std::span<const std::unique_ptr<pe::Layer>> siblings,
                                  std::size_t index) const {
    constexpr int kThumb = 26;
    if (doc_ == nullptr || index >= siblings.size()) return QIcon();
    const pe::Rect bounds = doc_->canvasBounds();
    const std::int64_t area = static_cast<std::int64_t>(bounds.width) * bounds.height;
    if (area <= 0 || area > 4'000'000) return QIcon();  // bound preview-composite cost

    // Composite this layer alone over the canvas, then fit it onto a small checker.
    const pe::PixelBuffer buf = pe::compositeToImage(siblings.subspan(index, 1), bounds);
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

QTreeWidgetItem* LayersPanel::itemForId(pe::LayerId id) const {
    if (id == pe::kNoLayer) return nullptr;
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if (idOf(*it) == id) return *it;
    }
    return nullptr;
}

void LayersPanel::updateLayerThumbnail(pe::LayerId id) {
    if (doc_ == nullptr || id == pe::kNoLayer) {
        rebuild();
        return;
    }
    QTreeWidgetItem* item = itemForId(id);
    if (item == nullptr) {
        rebuild();  // tree out of sync: rebuild it
        return;
    }
    // Find the engine slot (sibling span + index) that directly owns `id` so we can
    // composite just that layer. Pixels only change on non-group layers.
    std::span<const std::unique_ptr<pe::Layer>> level = doc_->topLevelLayers();
    std::size_t idx = pe::GroupLayer::npos;
    for (;;) {
        bool descended = false;
        for (std::size_t i = 0; i < level.size(); ++i) {
            const pe::Layer* l = level[i].get();
            if (l == nullptr) continue;
            if (l->id() == id) {
                idx = i;
                break;
            }
            if (l->kind() == pe::LayerKind::Group &&
                static_cast<const pe::GroupLayer*>(l)->findDescendant(id) != nullptr) {
                level = static_cast<const pe::GroupLayer*>(l)->children();
                descended = true;
                break;
            }
        }
        if (idx != pe::GroupLayer::npos || !descended) break;
    }
    if (idx == pe::GroupLayer::npos) {
        rebuild();
        return;
    }
    // Guard: setIcon emits itemChanged, which must not be read back as an edit.
    updating_ = true;
    // Choose the column-0 icon by kind, exactly like addLevel: a group keeps its folder glyph and
    // an adjustment keeps its glyph (compositing them alone would blank the icon); only pixel/text
    // layers get a live composited thumbnail. (Reached by both the pixel-paint and mask-paint
    // paths.)
    const pe::Layer* l = level[idx].get();
    QIcon icon;
    if (l != nullptr && l->kind() == pe::LayerKind::Group) {
        icon = groupIcon();
    } else if (l != nullptr && l->isAdjustment()) {
        icon = adjustmentIcon();
    } else {
        icon = layerThumbnail(level, idx);
    }
    item->setIcon(0, icon);
    updating_ = false;
}

void LayersPanel::updateMaskThumbnail(pe::LayerId id) {
    // Refresh just one row's column-1 mask thumbnail (preserving the focus ring) after a mask edit,
    // instead of rebuilding the whole tree. Falls back to rebuild() if the row is out of sync.
    if (doc_ == nullptr || id == pe::kNoLayer) return;
    QTreeWidgetItem* item = itemForId(id);
    if (item == nullptr) {
        rebuild();
        return;
    }
    const pe::Layer* l = doc_->findLayer(id);
    if (l == nullptr || l->mask() == nullptr) return;
    updating_ = true;
    item->setIcon(1, maskThumbnail(*l->mask(), id == maskTarget_));
    updating_ = false;
}

void LayersPanel::addLevel(QTreeWidgetItem* parentItem,
                           std::span<const std::unique_ptr<pe::Layer>> siblings) {
    // Display top layer first: walk the stack from top (highest index) down.
    for (std::size_t i = siblings.size(); i-- > 0;) {
        const pe::Layer* l = siblings[i].get();
        if (l == nullptr) continue;
        const bool isGroup = l->kind() == pe::LayerKind::Group;
        auto* item = new QTreeWidgetItem();
        item->setText(0, QString::fromStdString(l->name()));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, l->visible() ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, static_cast<qulonglong>(l->id()));
        // Adjustment layers contribute no pixels (a composited thumbnail would be blank), so they
        // get the adjustment glyph; groups get the folder glyph; pixel layers get a live thumbnail.
        item->setIcon(0, isGroup             ? groupIcon()
                         : l->isAdjustment() ? adjustmentIcon()
                                             : layerThumbnail(siblings, i));
        // Column 1: a grayscale mask thumbnail when the layer carries a mask (with a focus ring
        // when it is the current mask-edit target).
        if (l->mask() != nullptr)
            item->setIcon(1, maskThumbnail(*l->mask(), l->id() == maskTarget_));
        if (parentItem != nullptr) {
            parentItem->addChild(item);
        } else {
            tree_->addTopLevelItem(item);
        }
        if (isGroup) {
            addLevel(item, static_cast<const pe::GroupLayer*>(l)->children());
            item->setExpanded(collapsed_.find(l->id()) == collapsed_.end());
        }
    }
}

void LayersPanel::rebuild() {
    updating_ = true;
    tree_->clear();
    if (doc_ != nullptr) {
        addLevel(nullptr, doc_->topLevelLayers());
        // Drop collapse state for groups that no longer exist (e.g. ungrouped/deleted), so
        // the set doesn't accumulate dead ids across a session.
        for (auto it = collapsed_.begin(); it != collapsed_.end();) {
            it = (itemForId(*it) == nullptr) ? collapsed_.erase(it) : std::next(it);
        }
        selectActiveInTree();
    }
    updating_ = false;
    syncActiveControls();
    updateButtons();
}

void LayersPanel::selectActiveInTree() {
    if (doc_ == nullptr) return;
    QTreeWidgetItem* item = itemForId(doc_->activeLayer());
    if (item != nullptr) tree_->setCurrentItem(item);  // sets current + selects it
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
    const pe::LayerId active = hasDoc ? doc_->activeLayer() : pe::kNoLayer;
    const pe::Layer* a = hasDoc ? doc_->findLayer(active) : nullptr;
    const bool hasActive = a != nullptr;
    const std::size_t n = hasDoc ? doc_->topLevelCount() : 0;
    const std::size_t idx = hasActive ? doc_->topLevelIndexOf(active) : pe::GroupLayer::npos;
    const bool activeTopLevel = idx != pe::GroupLayer::npos;

    addBtn_->setEnabled(hasDoc);
    // Duplicate / Delete / reorder operate on top-level layers only in this version.
    dupBtn_->setEnabled(hasActive && activeTopLevel);
    delBtn_->setEnabled(hasActive && activeTopLevel && n > 1);
    upBtn_->setEnabled(activeTopLevel && idx + 1 < n);
    downBtn_->setEnabled(activeTopLevel && idx > 0);
    groupBtn_->setEnabled(hasDoc && selectionAllTopLevel());
    ungroupBtn_->setEnabled(hasActive && activeTopLevel && a->kind() == pe::LayerKind::Group);
    maskBtn_->setEnabled(hasActive &&
                         a->mask() == nullptr);  // add a mask to a not-yet-masked layer
}

void LayersPanel::groupSelected() {
    if (doc_ == nullptr) return;
    std::vector<pe::LayerId> ids = selectedTopLevelIds();
    if (ids.empty()) return;
    // GroupLayersCommand validates the ids are distinct top-level siblings (which the
    // filter above guarantees) and sets the new group active, so the rebuild that the
    // resulting LayerStructure change triggers re-selects the group for us.
    push(std::make_unique<pe::GroupLayersCommand>(std::move(ids)));
}

void LayersPanel::ungroupSelected() {
    if (doc_ == nullptr) return;
    const pe::LayerId id = selectedLayer();
    const pe::Layer* l = doc_->findLayer(id);
    if (l == nullptr || l->kind() != pe::LayerKind::Group ||
        doc_->topLevelIndexOf(id) == pe::GroupLayer::npos) {
        return;  // only top-level groups dissolve in this version
    }
    // The command clears the active layer when the dissolved group was active; pick the
    // group's topmost child as the new active so the panel doesn't end up with nothing
    // selected. Captured before the command runs, while the group is still intact.
    pe::LayerId newActive = pe::kNoLayer;
    const auto kids = static_cast<const pe::GroupLayer*>(l)->children();
    if (!kids.empty() && kids.back() != nullptr) newActive = kids.back()->id();

    push(std::make_unique<pe::UngroupCommand>(id));
    // Only compensate when the command actually cleared the active layer (the dissolved
    // group was active). This avoids clobbering an unrelated active layer, and matches the
    // command's contract: undo restores the prior active, so we don't fight it.
    if (doc_->activeLayer() == pe::kNoLayer && newActive != pe::kNoLayer &&
        doc_->findLayer(newActive) != nullptr) {
        doc_->setActiveLayer(newActive);
    }
}

void LayersPanel::addMaskForActive() {
    if (doc_ == nullptr) return;
    const pe::LayerId id = doc_->activeLayer();
    const pe::Layer* l = doc_->findLayer(id);
    if (l == nullptr || l->mask() != nullptr) return;  // need a layer that has no mask yet
    // From the active selection if there is one (the common workflow), else a fully-revealing mask.
    const auto init = doc_->selection().active() ? pe::AddLayerMaskCommand::Init::FromSelection
                                                 : pe::AddLayerMaskCommand::Init::RevealAll;
    push(std::make_unique<pe::AddLayerMaskCommand>(id, init));
}

void LayersPanel::onItemClicked(QTreeWidgetItem* item, int column) {
    if (updating_ || doc_ == nullptr || item == nullptr) return;
    const pe::LayerId id = idOf(item);
    const pe::Layer* l = doc_->findLayer(id);
    if (column == 1 && l != nullptr && l->mask() != nullptr) {
        // A click on the mask thumbnail: Alt toggles enabled (the old gesture), a plain click
        // TARGETS the mask so the Brush paints into it (Photoshop: click the mask to edit it).
        if ((QGuiApplication::keyboardModifiers() & Qt::AltModifier) != 0) {
            push(std::make_unique<pe::SetMaskEnabledCommand>(id, !l->mask()->enabled()));
            return;
        }
        if (doc_->activeLayer() != id) {
            // Guard like onRowChanged: the resulting ActiveLayer notification must not re-select
            // and collapse a multi-selection (the #89 class of bug). Usually a no-op here — a real
            // click already moved the active layer via currentItemChanged -> onRowChanged.
            syncingActive_ = true;
            doc_->setActiveLayer(id);  // edit the mask of THIS layer
            syncingActive_ = false;
        }
        setMaskTarget(id);
        return;
    }
    // Any other click (the layer thumbnail/name, or a layer with no mask) leaves mask-edit mode.
    setMaskTarget(pe::kNoLayer);
}

void LayersPanel::onRowChanged() {
    if (updating_ || doc_ == nullptr) return;
    const pe::LayerId id = selectedLayer();
    if (id != pe::kNoLayer) {
        // The active change below mirrors the current row; mark it self-induced so the
        // resulting ActiveLayer notification does not re-select (and thereby collapse a
        // multi-selection the user is building for Group).
        syncingActive_ = true;
        doc_->setActiveLayer(id);  // session state (not undoable)
        syncingActive_ = false;
        // The mask target is reconciled centrally in onDocumentChanged's ActiveLayer branch (which
        // fires from the setActiveLayer above), so a row change that moves the active layer drops a
        // stale target there; the mask-thumbnail click path re-targets afterward in onItemClicked.
    }
}

void LayersPanel::setMaskTarget(pe::LayerId id) {
    // Only a layer that actually has a mask can be targeted; otherwise fall back to "no target".
    if (id != pe::kNoLayer) {
        const pe::Layer* l = doc_ != nullptr ? doc_->findLayer(id) : nullptr;
        if (l == nullptr || l->mask() == nullptr) id = pe::kNoLayer;
    }
    if (maskTarget_ == id) return;
    maskTarget_ = id;
    refreshMaskIcons();  // move the focus ring to the new target (cheap; no tree rebuild)
    emit maskEditTargetChanged(id != pe::kNoLayer);
}

void LayersPanel::clearMaskTarget() {
    setMaskTarget(pe::kNoLayer);
}

void LayersPanel::refreshMaskIcons() {
    if (doc_ == nullptr) return;
    // setIcon emits itemChanged; guard so it isn't read back as a visibility edit.
    updating_ = true;
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        const pe::Layer* l = doc_->findLayer(idOf(*it));
        if (l != nullptr && l->mask() != nullptr)
            (*it)->setIcon(1, maskThumbnail(*l->mask(), l->id() == maskTarget_));
    }
    updating_ = false;
}

void LayersPanel::onSelectionChanged() {
    if (updating_) return;
    updateButtons();  // Group enablement depends on the whole multi-selection
}

void LayersPanel::onItemChanged(QTreeWidgetItem* item, int /*column*/) {
    if (updating_ || doc_ == nullptr || item == nullptr) return;
    const pe::LayerId id = idOf(item);
    const pe::Layer* l = doc_->findLayer(id);
    if (l == nullptr) return;
    const bool want = item->checkState(0) == Qt::Checked;
    if (l->visible() != want) push(std::make_unique<pe::SetVisibilityCommand>(id, want));
}

void LayersPanel::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    if (doc_ == nullptr || item == nullptr) return;
    if (column != 0) return;  // the editors belong to the layer (col 0), not the mask thumb (col 1)
    const pe::LayerId id = idOf(item);
    const pe::Layer* l = doc_->findLayer(id);
    // Double-clicking an adjustment-layer row opens its parameter dialog; a text-layer row reopens
    // the text dialog. (MainWindow handles both.)
    if (l == nullptr) return;
    if (l->isAdjustment()) {
        emit editAdjustmentRequested(id);
    } else if (l->kind() == pe::LayerKind::Text) {
        emit editTextRequested(id);
    }
}

void LayersPanel::onItemExpanded(QTreeWidgetItem* item) {
    if (updating_) return;
    collapsed_.erase(idOf(item));
}

void LayersPanel::onItemCollapsed(QTreeWidgetItem* item) {
    if (updating_) return;
    const pe::LayerId id = idOf(item);
    if (id != pe::kNoLayer) collapsed_.insert(id);
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
    // DuplicateLayerCommand clones a top-level layer only; skip otherwise so no phantom
    // (no-op) undo entry is recorded.
    if (doc_->topLevelIndexOf(id) == pe::GroupLayer::npos) return;
    push(std::make_unique<pe::DuplicateLayerCommand>(id));
}

void LayersPanel::onDelete() {
    if (doc_ == nullptr || doc_->topLevelCount() <= 1) return;
    const pe::LayerId id = doc_->activeLayer();
    // RemoveLayerCommand removes a top-level layer only; skip otherwise (no phantom entry).
    if (doc_->topLevelIndexOf(id) == pe::GroupLayer::npos) return;
    push(std::make_unique<pe::RemoveLayerCommand>(id));
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
