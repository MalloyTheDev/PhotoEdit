#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"

#include <cstdint>
#include "pe/core/SolidColorLayer.hpp"
#include "pe/core/TextLayer.hpp"

#include "pe/core/Brush.hpp"  // PaintCommand (CropCommand composes per-layer moves)
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"      // moveLayerContent
#include "pe/core/GroupLayer.hpp"  // recurse into groups for crop
#include "pe/core/Mask.hpp"        // Mask, MaskBuffer, maskFromSelection (layer-mask commands)
#include "pe/core/PixelLayer.hpp"  // contentBounds() on the concrete pixel layer

#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace pe {

namespace {

// A LayerProps change for a property edit, carrying the layer's affected region.
DocumentChange propsChange(const Layer* layer, LayerId id) {
    DocumentChange c{DocumentChange::Kind::LayerProps, Rect{}, id};
    if (layer != nullptr) c.dirtyRegion = layer->contentBounds();
    return c;
}

DocumentChange structureChange(Rect region, LayerId id) {
    return DocumentChange{DocumentChange::Kind::LayerStructure, region, id};
}

// Collect every PIXEL layer id in the tree (descending into groups), for the crop content
// shift — a group's pixel children live in document space and must move with everything else.
// Budget for shifting a mask during a crop.
//
// It must not be TIGHTER than the pixel-move budget, or a crop whose pixel shift succeeds is
// refused as a whole the moment any layer carries a mask. That is what happened when the move
// budget moved to kMaxMoveBytes and this one stayed at kMaxFilterPixels: a 5000x5000 document
// could have its pixels shifted and not its masks, so one mask made the crop refuse and the
// #180 fix evaporated.
//
// Expressed in the same terms as the move budget, at a mask's one byte per pixel, so the two
// stay tied together rather than being two numbers someone has to remember to update.
constexpr int64_t kMaxCropGeometryPixels = kMaxMoveBytes;  // 1 byte per pixel of coverage

// Every layer in the tree, groups included. collectPixelLayers deliberately returns
// only paintable leaves; geometry shifting has to consider masks (which live on the
// base Layer, so any kind can carry one), text origins and fill bounds.
void collectAllLayers(std::span<const std::unique_ptr<Layer>> layers, std::vector<LayerId>& out) {
    for (const auto& l : layers) {
        if (l == nullptr) continue;
        out.push_back(l->id());
        if (l->kind() == LayerKind::Group) {
            collectAllLayers(static_cast<const GroupLayer*>(l.get())->children(), out);
        }
    }
}

void collectPixelLayers(std::span<const std::unique_ptr<Layer>> layers, std::vector<LayerId>& out) {
    for (const auto& l : layers) {
        if (l == nullptr) continue;
        if (l->kind() == LayerKind::Group) {
            collectPixelLayers(static_cast<const GroupLayer*>(l.get())->children(), out);
        } else if (l->kind() == LayerKind::Pixel) {
            out.push_back(l->id());
        }
    }
}

// Return a copy of `sel` translated by (dx,dy). An inactive selection has no coverage to move,
// so it is returned unchanged. toMask/loadMask guard the coordinate ranges, so a translation that
// pushes the bounds negative / off-canvas never overflows or crashes (off-canvas coords are
// permitted; out-of-range ones simply select nothing).
Selection translatedSelection(const Selection& sel, int dx, int dy) {
    if (!sel.active()) return sel;
    const Rect bounds = sel.tightBounds();
    if (bounds.isEmpty()) return sel;  // active but no coverage: nothing to move
    const PixelBuffer mask = sel.toMask(bounds);
    // toMask returns empty when the bounding box exceeds the selection cap (a sparse selection
    // can have a huge bbox but tiny coverage). Don't translate via a mask round-trip in that case
    // — loading an empty mask would silently deactivate the selection; keep it as-is instead.
    if (mask.isEmpty()) return sel;
    Selection out;
    out.loadMask(mask, bounds.left() + dx, bounds.top() + dy);
    return out;
}

}  // namespace

// ----------------------------------------------------------------- AddLayer

AddLayerCommand::AddLayerCommand(std::unique_ptr<Layer> layer, std::size_t index)
    : owned_(std::move(layer)), index_(index) {
    if (owned_) layerId_ = owned_->id();
}

DocumentChange AddLayerCommand::execute(Document& doc) {
    Rect region;
    if (owned_) region = owned_->contentBounds();
    doc.cmdInsertTopLevel(index_, std::move(owned_));  // owned_ now empty
    return structureChange(region, layerId_);
}

DocumentChange AddLayerCommand::undo(Document& doc) {
    owned_ = doc.cmdRemoveTopLevel(layerId_);
    Rect region;
    if (owned_) region = owned_->contentBounds();
    return structureChange(region, layerId_);
}

// -------------------------------------------------------------- RemoveLayer

RemoveLayerCommand::RemoveLayerCommand(LayerId id) : layerId_(id) {}

DocumentChange RemoveLayerCommand::execute(Document& doc) {
    index_ = doc.topLevelIndexOf(layerId_);
    prevActive_ = doc.activeLayer();
    owned_ = doc.cmdRemoveTopLevel(layerId_);
    // Clear the active layer if it was removed — either the removed layer itself OR a
    // descendant of a removed group (findLayer searches the whole tree). Otherwise a
    // stale active id would dangle past the destroyed subtree.
    clearedActive_ = prevActive_ != kNoLayer && doc.findLayer(prevActive_) == nullptr;
    if (clearedActive_) doc.setActiveLayer(kNoLayer);
    Rect region;
    if (owned_) region = owned_->contentBounds();
    return structureChange(region, layerId_);
}

DocumentChange RemoveLayerCommand::undo(Document& doc) {
    Rect region;
    if (owned_) region = owned_->contentBounds();
    doc.cmdInsertTopLevel(index_, std::move(owned_));
    // The removed subtree (with its original ids) is back, so the prior active layer
    // exists again; restore it if this command had cleared it.
    if (clearedActive_) doc.setActiveLayer(prevActive_);
    return structureChange(region, layerId_);
}

// ----------------------------------------------------------- MergeLayers

MergeBlock mergeBlocker(const Document& doc, std::span<const std::size_t> indices) {
    if (indices.size() < 2) return MergeBlock::TooFewLayers;
    const std::span<const std::unique_ptr<Layer>> top = doc.topLevelLayers();
    for (std::size_t i : indices) {
        if (i >= top.size() || top[i] == nullptr) return MergeBlock::TooFewLayers;
    }
    // The lowest layer's clipping base sits below the set. Merging would render it unclipped
    // and the picture would change, which is the one thing a merge must not do.
    if (top[indices.front()]->clipped()) return MergeBlock::LowestIsClipped;
    const Rect canvas = doc.canvasBounds();
    if (canvas.isEmpty()) return MergeBlock::TooFewLayers;
    // compositeToImage returns an empty buffer past this, and a merge that composited nothing
    // would erase every layer it took.
    if (static_cast<int64_t>(canvas.width) * static_cast<int64_t>(canvas.height) >
        kMaxCompositeImagePixels) {
        return MergeBlock::OverCompositeCap;
    }
    return MergeBlock::None;
}

MergeLayersCommand::MergeLayersCommand(std::vector<std::size_t> indices, std::string name,
                                       std::string mergedLayerName)
    : indices_(std::move(indices)),
      name_(std::move(name)),
      mergedLayerName_(std::move(mergedLayerName)) {
    std::sort(indices_.begin(), indices_.end());
    indices_.erase(std::unique(indices_.begin(), indices_.end()), indices_.end());
}

MergeLayersCommand::~MergeLayersCommand() = default;

namespace {

// Resident bytes of a detached layer subtree: its tiles at its own depth, its mask, and the
// same for every descendant. Approximate and deliberately over-counting, like every other
// retainedBytes in the engine: over-counting trims history sooner, under-counting is how a
// budget gets silently blown.
[[nodiscard]] std::int64_t layerResidentBytes(const Layer* layer) {
    if (layer == nullptr) return 0;
    const auto perTile = static_cast<std::int64_t>(kTilePixels);
    std::int64_t bytes = 0;
    if (const auto* pl = dynamic_cast<const PixelLayer*>(layer); pl != nullptr) {
        switch (pl->depth()) {
            case BitDepth::U16:
                bytes += static_cast<std::int64_t>(pl->tiles16().tileCount()) * perTile * 8;
                break;
            case BitDepth::F32:
                bytes += static_cast<std::int64_t>(pl->tilesF().tileCount()) * perTile * 16;
                break;
            case BitDepth::U8:
            default:
                bytes += static_cast<std::int64_t>(pl->tiles().tileCount()) * perTile * 4;
                break;
        }
    }
    if (const Mask* m = layer->mask(); m != nullptr) {
        bytes += static_cast<std::int64_t>(m->buffer().tileCount()) * perTile;  // 1 byte/px
    }
    if (const auto* g = dynamic_cast<const GroupLayer*>(layer); g != nullptr) {
        for (const std::unique_ptr<Layer>& child : g->children()) {
            bytes += layerResidentBytes(child.get());
        }
    }
    return bytes;
}

}  // namespace

std::int64_t MergeLayersCommand::retainedBytes() const noexcept {
    std::int64_t bytes = 0;
    for (const std::unique_ptr<Layer>& l : removed_) bytes += layerResidentBytes(l.get());
    return bytes;
}

DocumentChange MergeLayersCommand::execute(Document& doc) {
    // Re-executed after an undo: the originals are back in the document, so start over rather
    // than reusing the previous run's state.
    removed_.clear();
    mergedId_ = kNoLayer;

    if (mergeBlocker(doc, indices_) != MergeBlock::None) return structureChange(Rect{}, kNoLayer);

    const Rect canvas = doc.canvasBounds();
    prevActive_ = doc.activeLayer();

    // Take the layers out in ASCENDING order and keep them: they are both what gets
    // composited and exactly what undo has to put back. Removing from the highest index down
    // keeps the lower indices valid while the removals happen.
    const std::span<const std::unique_ptr<Layer>> top = doc.topLevelLayers();
    std::vector<LayerId> ids;
    ids.reserve(indices_.size());
    for (std::size_t i : indices_) ids.push_back(top[i]->id());
    removed_.resize(ids.size());
    for (std::size_t k = ids.size(); k-- > 0;) removed_[k] = doc.cmdRemoveTopLevel(ids[k]);

    // Composited by the same code that draws the canvas, so a merged layer looks like what it
    // replaced rather than like a second implementation's idea of it.
    const PixelBuffer flat = compositeToImage(removed_, canvas);
    if (flat.isEmpty()) {
        // Put everything back and report nothing: an empty composite here would mean merging
        // to a blank layer.
        for (std::size_t k = 0; k < indices_.size(); ++k) {
            doc.cmdInsertTopLevel(indices_[k], std::move(removed_[k]));
        }
        removed_.clear();
        return structureChange(Rect{}, kNoLayer);
    }

    auto layer = std::make_unique<PixelLayer>(mergedLayerName_);
    for (int y = 0; y < flat.height(); ++y) {
        for (int x = 0; x < flat.width(); ++x) {
            const Rgba8 px = flat.at(x, y);
            // Sparse: a merged layer of a small shape must not allocate a tile per canvas
            // tile just to hold transparency.
            if (px.a != 0) layer->tiles().setPixel(canvas.x + x, canvas.y + y, px);
        }
    }
    mergedId_ = layer->id();
    doc.cmdInsertTopLevel(indices_.front(), std::move(layer));
    // The layers the active one may have been inside are gone; land on the survivor, which is
    // where the user's attention is.
    doc.setActiveLayer(mergedId_);
    return structureChange(canvas, mergedId_);
}

DocumentChange MergeLayersCommand::undo(Document& doc) {
    if (mergedId_ == kNoLayer) return structureChange(Rect{}, kNoLayer);
    const Rect canvas = doc.canvasBounds();
    (void)doc.cmdRemoveTopLevel(mergedId_);
    // Ascending, so each insert lands at the index it was taken from: the lower ones are
    // already back by the time a higher index is used.
    for (std::size_t k = 0; k < indices_.size(); ++k) {
        doc.cmdInsertTopLevel(indices_[k], std::move(removed_[k]));
    }
    removed_.clear();
    mergedId_ = kNoLayer;
    if (prevActive_ != kNoLayer && doc.findLayer(prevActive_) != nullptr) {
        doc.setActiveLayer(prevActive_);
    }
    return structureChange(canvas, prevActive_);
}

std::vector<std::size_t> mergeDownIndices(const Document& doc, LayerId active) {
    const std::size_t idx = doc.topLevelIndexOf(active);
    // Index 0 is the bottom of the stack, so there is nothing under it to merge into.
    if (idx == GroupLayer::npos || idx == 0) return {};
    return {idx - 1, idx};
}

std::vector<std::size_t> mergeVisibleIndices(const Document& doc) {
    std::vector<std::size_t> out;
    const std::span<const std::unique_ptr<Layer>> top = doc.topLevelLayers();
    for (std::size_t i = 0; i < top.size(); ++i) {
        if (top[i] != nullptr && top[i]->visible()) out.push_back(i);
    }
    if (out.size() < 2) return {};
    return out;
}

std::vector<std::size_t> flattenIndices(const Document& doc) {
    const std::size_t n = doc.topLevelCount();
    if (n < 2) return {};
    std::vector<std::size_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = i;
    return out;
}

// ----------------------------------------------------------- DuplicateLayer

DuplicateLayerCommand::DuplicateLayerCommand(LayerId sourceId) : sourceId_(sourceId) {}

DocumentChange DuplicateLayerCommand::execute(Document& doc) {
    if (!owned_) {
        // First execution: clone the source (must be top-level in M1).
        const std::size_t srcIdx = doc.topLevelIndexOf(sourceId_);
        const Layer* src = doc.findLayer(sourceId_);
        if (src == nullptr || srcIdx == GroupLayer::npos) {
            return structureChange(Rect{}, kNoLayer);  // nothing to duplicate
        }
        owned_ = src->clone();
        cloneId_ = owned_->id();
        index_ = srcIdx + 1;
    }
    Rect region;
    if (owned_) region = owned_->contentBounds();
    doc.cmdInsertTopLevel(index_, std::move(owned_));
    return structureChange(region, cloneId_);
}

DocumentChange DuplicateLayerCommand::undo(Document& doc) {
    owned_ = doc.cmdRemoveTopLevel(cloneId_);
    Rect region;
    if (owned_) region = owned_->contentBounds();
    return structureChange(region, cloneId_);
}

// ------------------------------------------------------------ ReorderLayer

ReorderLayerCommand::ReorderLayerCommand(LayerId id, std::size_t newIndex)
    : layerId_(id), newIndex_(newIndex) {}

DocumentChange ReorderLayerCommand::execute(Document& doc) {
    oldIndex_ = doc.topLevelIndexOf(layerId_);
    if (oldIndex_ == GroupLayer::npos) return structureChange(Rect{}, layerId_);
    std::unique_ptr<Layer> layer = doc.cmdRemoveTopLevel(layerId_);
    Rect region;
    if (layer) region = layer->contentBounds();
    doc.cmdInsertTopLevel(newIndex_, std::move(layer));
    return structureChange(region, layerId_);
}

DocumentChange ReorderLayerCommand::undo(Document& doc) {
    if (oldIndex_ == GroupLayer::npos) return structureChange(Rect{}, layerId_);
    std::unique_ptr<Layer> layer = doc.cmdRemoveTopLevel(layerId_);
    Rect region;
    if (layer) region = layer->contentBounds();
    doc.cmdInsertTopLevel(oldIndex_, std::move(layer));
    return structureChange(region, layerId_);
}

// ------------------------------------------------------------- GroupLayers

GroupLayersCommand::GroupLayersCommand(std::vector<LayerId> ids) : members_(std::move(ids)) {}

DocumentChange GroupLayersCommand::execute(Document& doc) {
    if (!validated_) {
        validated_ = true;
        // Validate: at least one id, every id a DISTINCT top-level sibling. (v1 groups
        // top-level layers only; mixed-parent / nested / unknown / duplicate ids are
        // rejected so the tree is never corrupted.) Collect (index, id) for the valid set.
        std::vector<std::pair<std::size_t, LayerId>> indexed;
        indexed.reserve(members_.size());
        bool ok = !members_.empty();
        for (LayerId id : members_) {
            const std::size_t idx = doc.topLevelIndexOf(id);
            if (idx == GroupLayer::npos) {
                ok = false;  // not a top-level layer (unknown or nested)
                break;
            }
            // Reject duplicates: the same id must not appear twice.
            bool dup = false;
            for (const auto& [i, existing] : indexed) {
                if (existing == id) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                ok = false;
                break;
            }
            indexed.emplace_back(idx, id);
        }
        if (!ok) {
            noop_ = true;
            return structureChange(Rect{}, kNoLayer);
        }
        // Order members by their current top-level index so the group preserves their
        // relative stacking; the topmost (smallest index) is where the group is inserted.
        std::sort(indexed.begin(), indexed.end());
        members_.clear();
        oldIndices_.clear();
        members_.reserve(indexed.size());
        oldIndices_.reserve(indexed.size());
        for (const auto& [idx, id] : indexed) {
            members_.push_back(id);
            oldIndices_.push_back(idx);
        }
        insertIndex_ = oldIndices_.front();
        prevActive_ = doc.activeLayer();
        // Create the group shell once; it is reused across undo/redo so its id is stable.
        ownedGroup_ = std::make_unique<GroupLayer>("Group");
        groupId_ = ownedGroup_->id();
    }
    if (noop_) return structureChange(Rect{}, kNoLayer);
    // Defensive: the shell is created on first execute and round-tripped via undo, so it is
    // non-null under the History lockstep. Guard anyway — a command driven out of order must
    // never dereference null.
    if (!ownedGroup_) return structureChange(Rect{}, kNoLayer);

    // Move each member out of the top level (highest index first so the remaining indices
    // stay valid) and into the retained group shell. Re-add in member order afterwards so
    // the group's children keep the requested relative stacking.
    auto* group = static_cast<GroupLayer*>(ownedGroup_.get());
    std::vector<std::unique_ptr<Layer>> taken(members_.size());
    for (std::size_t k = members_.size(); k-- > 0;) {
        taken[k] = doc.cmdRemoveTopLevel(members_[k]);
    }
    for (auto& layer : taken) group->addChild(std::move(layer));

    const Rect region = group->contentBounds();
    doc.cmdInsertTopLevel(insertIndex_, std::move(ownedGroup_));  // ownedGroup_ now empty
    doc.setActiveLayer(groupId_);
    return structureChange(region, groupId_);
}

DocumentChange GroupLayersCommand::undo(Document& doc) {
    if (noop_) return structureChange(Rect{}, kNoLayer);

    // Take the group back out (it still owns the members), then splice each member back to
    // its exact original top-level index. Re-inserting in ascending original-index order
    // reproduces the original flat layout exactly (lower slots are filled before higher ones).
    ownedGroup_ = doc.cmdRemoveTopLevel(groupId_);
    if (!ownedGroup_)
        return structureChange(Rect{}, kNoLayer);  // group already gone; nothing to undo
    auto* group = static_cast<GroupLayer*>(ownedGroup_.get());
    const Rect region = group->contentBounds();
    for (std::size_t k = 0; k < members_.size(); ++k) {
        doc.cmdInsertTopLevel(oldIndices_[k], group->removeChild(members_[k]));
    }
    // The group is now an empty shell held by the command; restore the prior active layer.
    doc.setActiveLayer(prevActive_);
    return structureChange(region, groupId_);
}

// --------------------------------------------------------------- Ungroup

UngroupCommand::UngroupCommand(LayerId groupId) : groupId_(groupId) {}

DocumentChange UngroupCommand::execute(Document& doc) {
    if (!validated_) {
        validated_ = true;
        // Validate: a top-level GroupLayer. A non-group, unknown, or nested id is a no-op.
        const std::size_t idx = doc.topLevelIndexOf(groupId_);
        const Layer* g = doc.findLayer(groupId_);
        if (idx == GroupLayer::npos || g == nullptr || g->kind() != LayerKind::Group) {
            noop_ = true;
            return structureChange(Rect{}, kNoLayer);
        }
        groupIndex_ = idx;
        prevActive_ = doc.activeLayer();
        childIds_.clear();
        for (const auto& child : static_cast<const GroupLayer*>(g)->children()) {
            if (child) childIds_.push_back(child->id());
        }
    }
    if (noop_) return structureChange(Rect{}, kNoLayer);

    // Take the group out (keeping its shell for undo) and splice its children into the top
    // level at the group's slot, preserving order: inserting child k at groupIndex_ + k keeps
    // top-to-bottom order since earlier children already occupy the lower slots.
    ownedGroup_ = doc.cmdRemoveTopLevel(groupId_);
    if (!ownedGroup_)
        return structureChange(Rect{}, kNoLayer);  // group already gone; defensive no-op
    auto* group = static_cast<GroupLayer*>(ownedGroup_.get());
    const Rect region = group->contentBounds();
    for (std::size_t k = 0; k < childIds_.size(); ++k) {
        doc.cmdInsertTopLevel(groupIndex_ + k, group->removeChild(childIds_[k]));
    }
    // If the dissolved group was active, the active id now dangles; clear it (the group is
    // gone from the document). undo restores it.
    if (doc.activeLayer() == groupId_) doc.setActiveLayer(kNoLayer);
    return structureChange(region, groupId_);
}

DocumentChange UngroupCommand::undo(Document& doc) {
    if (noop_) return structureChange(Rect{}, kNoLayer);

    // Pull the children back out of the top level (highest slot first so indices stay valid)
    // and return them to the retained group shell in their original order, then re-insert the
    // rebuilt group at its slot. Same id, same children, same order.
    if (!ownedGroup_) return structureChange(Rect{}, kNoLayer);  // execute never captured the shell
    auto* group = static_cast<GroupLayer*>(ownedGroup_.get());
    std::vector<std::unique_ptr<Layer>> taken(childIds_.size());
    for (std::size_t k = childIds_.size(); k-- > 0;) {
        taken[k] = doc.cmdRemoveTopLevel(childIds_[k]);
    }
    for (auto& layer : taken) group->addChild(std::move(layer));

    const Rect region = group->contentBounds();
    doc.cmdInsertTopLevel(groupIndex_, std::move(ownedGroup_));  // ownedGroup_ now empty
    doc.setActiveLayer(prevActive_);
    return structureChange(region, groupId_);
}

// ----------------------------------------------------------- property edits

SetVisibilityCommand::SetVisibilityCommand(LayerId id, bool visible)
    : layerId_(id), newVisible_(visible) {}

DocumentChange SetVisibilityCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer == nullptr) return propsChange(nullptr, layerId_);
    oldVisible_ = layer->visible();
    layer->setVisible(newVisible_);
    return propsChange(layer, layerId_);
}

DocumentChange SetVisibilityCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer != nullptr) layer->setVisible(oldVisible_);
    return propsChange(layer, layerId_);
}

SetOpacityCommand::SetOpacityCommand(LayerId id, float opacity)
    : layerId_(id), newOpacity_(opacity) {}

DocumentChange SetOpacityCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer == nullptr) return propsChange(nullptr, layerId_);
    oldOpacity_ = layer->opacity();
    layer->setOpacity(newOpacity_);
    return propsChange(layer, layerId_);
}

DocumentChange SetOpacityCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer != nullptr) layer->setOpacity(oldOpacity_);
    return propsChange(layer, layerId_);
}

SetFillOpacityCommand::SetFillOpacityCommand(LayerId id, float fillOpacity)
    : layerId_(id), newFill_(fillOpacity) {}

DocumentChange SetFillOpacityCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer == nullptr) return propsChange(nullptr, layerId_);
    oldFill_ = layer->fillOpacity();
    layer->setFillOpacity(newFill_);
    return propsChange(layer, layerId_);
}

DocumentChange SetFillOpacityCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer != nullptr) layer->setFillOpacity(oldFill_);
    return propsChange(layer, layerId_);
}

SetClippedCommand::SetClippedCommand(LayerId id, bool clipped)
    : layerId_(id), newClipped_(clipped) {}

DocumentChange SetClippedCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer == nullptr) return propsChange(nullptr, layerId_);
    oldClipped_ = layer->clipped();
    layer->setClipped(newClipped_);
    return propsChange(layer, layerId_);
}

DocumentChange SetClippedCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer != nullptr) layer->setClipped(oldClipped_);
    return propsChange(layer, layerId_);
}

SetBlendModeCommand::SetBlendModeCommand(LayerId id, BlendMode mode)
    : layerId_(id), newMode_(mode) {}

DocumentChange SetBlendModeCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer == nullptr) return propsChange(nullptr, layerId_);
    oldMode_ = layer->blendMode();
    layer->setBlendMode(newMode_);
    return propsChange(layer, layerId_);
}

DocumentChange SetBlendModeCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer != nullptr) layer->setBlendMode(oldMode_);
    return propsChange(layer, layerId_);
}

RenameLayerCommand::RenameLayerCommand(LayerId id, std::string name)
    : layerId_(id), newName_(std::move(name)) {}

DocumentChange RenameLayerCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer == nullptr) return propsChange(nullptr, layerId_);
    oldName_ = layer->name();
    layer->setName(newName_);
    return propsChange(layer, layerId_);
}

DocumentChange RenameLayerCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (layer != nullptr) layer->setName(oldName_);
    return propsChange(layer, layerId_);
}

// --------------------------------------------------------------- Layer masks

AddLayerMaskCommand::AddLayerMaskCommand(LayerId id, Init init) : layerId_(id), init_(init) {}

DocumentChange AddLayerMaskCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (!validated_) {
        validated_ = true;
        // Build the mask ONCE (so redo re-attaches the same object, preserving any painted
        // content).
        if (layer == nullptr || layer->mask() != nullptr) {
            noop_ = true;  // missing layer, or already masked: nothing to do
        } else {
            switch (init_) {
                case Init::FromSelection:
                    // An active selection needs a materialized mask; if the canvas is too large for
                    // the mask buffer, don't attach a misleadingly empty (reveal-all) one — no-op.
                    // (An inactive selection legitimately yields a reveal-all mask, which always
                    // fits.)
                    if (doc.selection().active() && !maskFillFits(doc.canvasBounds())) {
                        noop_ = true;
                    } else {
                        owned_ = std::make_unique<Mask>(
                            maskFromSelection(doc.selection(), doc.canvasBounds()));
                    }
                    break;
                case Init::HideAll:
                    // Hide-all must materialize a fully-cleared buffer; refuse rather than silently
                    // attach a reveal-all mask (the inverse of the intent) on an over-large canvas.
                    if (!maskFillFits(doc.canvasBounds())) {
                        noop_ = true;
                    } else {
                        owned_ = std::make_unique<Mask>(Mask::Kind::Layer);
                        owned_->buffer().fillRect(doc.canvasBounds(), MaskBuffer::kClear);
                    }
                    break;
                case Init::RevealAll:
                default:
                    owned_ = std::make_unique<Mask>(Mask::Kind::Layer);  // empty buffer reveals all
                    break;
            }
        }
    }
    if (noop_ || layer == nullptr) return propsChange(layer, layerId_);
    if (owned_) layer->setMask(std::move(owned_));  // owned_ now empty until undo
    return propsChange(layer, layerId_);
}

DocumentChange AddLayerMaskCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (noop_ || layer == nullptr) return propsChange(layer, layerId_);
    owned_ = layer->takeMask();  // detach and retain for redo
    return propsChange(layer, layerId_);
}

RemoveLayerMaskCommand::RemoveLayerMaskCommand(LayerId id) : layerId_(id) {}

DocumentChange RemoveLayerMaskCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (!validated_) {
        validated_ = true;
        noop_ = layer == nullptr || layer->mask() == nullptr;  // nothing to remove
    }
    if (noop_ || layer == nullptr) return propsChange(layer, layerId_);
    owned_ = layer->takeMask();  // retain for undo
    return propsChange(layer, layerId_);
}

DocumentChange RemoveLayerMaskCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    if (noop_ || layer == nullptr) return propsChange(layer, layerId_);
    if (owned_) layer->setMask(std::move(owned_));
    return propsChange(layer, layerId_);
}

SetMaskEnabledCommand::SetMaskEnabledCommand(LayerId id, bool enabled)
    : layerId_(id), newEnabled_(enabled) {}

DocumentChange SetMaskEnabledCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    Mask* mask = layer != nullptr ? layer->mask() : nullptr;
    if (!validated_) {
        validated_ = true;
        if (mask == nullptr) {
            noop_ = true;
        } else {
            oldEnabled_ = mask->enabled();
        }
    }
    if (noop_ || mask == nullptr) return propsChange(layer, layerId_);
    mask->setEnabled(newEnabled_);
    return propsChange(layer, layerId_);
}

DocumentChange SetMaskEnabledCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layerId_);
    Mask* mask = layer != nullptr ? layer->mask() : nullptr;
    if (noop_ || mask == nullptr) return propsChange(layer, layerId_);
    mask->setEnabled(oldEnabled_);
    return propsChange(layer, layerId_);
}

AssignProfileCommand::AssignProfileCommand(ColorProfileRef profile)
    : newProfile_(std::move(profile)) {}

DocumentChange AssignProfileCommand::execute(Document& doc) {
    if (!captured_) {
        oldProfile_ = doc.colorProfile();  // remember the prior tag for undo (once)
        captured_ = true;
    }
    doc.cmdSetColorProfile(newProfile_);
    return DocumentChange{DocumentChange::Kind::Profile, Rect{}, kNoLayer};
}

DocumentChange AssignProfileCommand::undo(Document& doc) {
    doc.cmdSetColorProfile(oldProfile_);
    return DocumentChange{DocumentChange::Kind::Profile, Rect{}, kNoLayer};
}

// --- Selection commands (task 12) ---

SetSelectionCommand::SetSelectionCommand(Selection target) : newSel_(std::move(target)) {}

DocumentChange SetSelectionCommand::execute(Document& doc) {
    if (!captured_) {
        // Capture the prior selection once, for undo. Moved rather than copied: the very
        // next line overwrites the live selection anyway, and the mask can be hundreds of
        // megabytes on a large canvas. Redo skips this, so oldSel_ stays the ORIGINAL.
        oldSel_ = std::move(doc.editableSelection());
        captured_ = true;
    }
    // Mutate the selection, then let History notify the returned Selection change ONCE. (Don't also
    // call touchSelection() — that would self-notify Selection a second time for the same change.
    // CropCommand, by contrast, returns LayerStructure and legitimately self-notifies Selection on
    // top, because those are two distinct kinds it must broadcast.)
    doc.editableSelection() = newSel_;
    return DocumentChange{DocumentChange::Kind::Selection, Rect{}, kNoLayer};
}

DocumentChange SetSelectionCommand::undo(Document& doc) {
    doc.editableSelection() = oldSel_;
    return DocumentChange{DocumentChange::Kind::Selection, Rect{}, kNoLayer};
}

// ------------------------------------------------------------- Reframe

ReframeCommand::~ReframeCommand() = default;

void ReframeCommand::shiftGeometry(Document& doc, int dx, int dy) {
    // Shift every non-pixel document-space placement. Pixel content is handled by moves_; these
    // are the geometries a reframe would otherwise leave behind, making masks, glyphs and fills
    // line up with the wrong pixels. Exactly invertible, so undo passes the opposite delta.
    if (dx == 0 && dy == 0) return;
    for (LayerId id : maskLayers_) {
        Layer* l = doc.findLayer(id);
        if (l == nullptr || l->mask() == nullptr) continue;
        // Already proven shiftable during capture, and translate is exactly invertible, so the
        // undo direction cannot fail either.
        (void)l->mask()->buffer().translate(dx, dy, kMaxCropGeometryPixels);
    }
    for (LayerId id : textLayers_) {
        Layer* l = doc.findLayer(id);
        if (l == nullptr || l->kind() != LayerKind::Text) continue;
        auto* t = static_cast<TextLayer*>(l);
        const Point o = t->rasterOrigin();
        t->setRasterOrigin(Point{o.x + dx, o.y + dy});
    }
    for (LayerId id : fillLayers_) {
        Layer* l = doc.findLayer(id);
        if (l == nullptr || l->kind() != LayerKind::Fill) continue;
        auto* f = static_cast<SolidColorLayer*>(l);
        const Rect b = f->bounds();
        f->setBounds(Rect{b.x + dx, b.y + dy, b.width, b.height});
    }
}

DocumentChange ReframeCommand::execute(Document& doc) {
    if (!captured_) {
        captured_ = true;
        oldSize_ = doc.canvasSize();
        oldSel_ = doc.selection();  // snapshot for undo (once); shifted on every execute below

        Size planned{};
        int pdx = 0;
        int pdy = 0;
        if (!plan(doc, planned, pdx, pdy)) {
            noop_ = true;
        } else {
            newSize_ = planned;
            dx_ = pdx;
            dy_ = pdy;
            const bool needShift = dx_ != 0 || dy_ != 0;
            std::vector<LayerId> pixelLayers;
            collectPixelLayers(doc.topLevelLayers(), pixelLayers);  // incl. nested-in-group
            // Build (but don't yet apply) a content shift for every pixel layer, so each move
            // snapshots the ORIGINAL pixels. A zero shift / empty layer yields null.
            bool shiftable = true;
            for (LayerId id : pixelLayers) {
                if (auto m = moveLayerContent(doc, id, dx_, dy_)) {
                    moves_.push_back(std::move(m));
                } else if (needShift) {
                    // A null with a real shift means either an empty layer (fine to skip) or a
                    // layer whose content exceeds the move budget. The latter would leave the
                    // doc half-reframed (resized but content not moved), so refuse outright.
                    const Layer* l = doc.findLayer(id);
                    if (l != nullptr &&
                        !static_cast<const PixelLayer*>(l)->contentBounds().isEmpty()) {
                        shiftable = false;
                        break;
                    }
                }
            }
            // Non-pixel document-space geometry: masks (on any layer kind), text raster origins
            // and fill bounds. Checked before anything is applied so an unshiftable mask refuses
            // the whole reframe rather than half-applying it.
            if (shiftable) {
                std::vector<LayerId> all;
                collectAllLayers(doc.topLevelLayers(), all);
                for (LayerId id : all) {
                    const Layer* l = doc.findLayer(id);
                    if (l == nullptr) continue;
                    const Mask* m = l->mask();
                    if (m != nullptr && !m->buffer().empty()) {
                        if (!m->buffer().canTranslate(dx_, dy_, kMaxCropGeometryPixels)) {
                            shiftable = false;
                            break;
                        }
                        maskLayers_.push_back(id);
                    }
                    if (l->kind() == LayerKind::Text) {
                        textLayers_.push_back(id);
                    } else if (l->kind() == LayerKind::Fill) {
                        fillLayers_.push_back(id);
                    }
                }
            }
            if (!shiftable) {
                moves_.clear();  // none executed yet; abort to a no-op rather than half-apply
                maskLayers_.clear();
                textLayers_.clear();
                fillLayers_.clear();
                noop_ = true;
            } else {
                noop_ = false;
            }
        }
    }
    if (noop_) return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};

    for (auto& m : moves_) m->execute(doc);
    shiftGeometry(doc, dx_, dy_);
    doc.cmdSetCanvasSize(newSize_);
    // Shift the active selection by the same offset so it tracks the content. Recomputed from
    // the captured original each time, so redo is exact (re-shifting an inactive sel is a
    // no-op). Notify observers (marching ants) just as SetSelectionCommand does.
    doc.editableSelection() = translatedSelection(oldSel_, dx_, dy_);
    doc.touchSelection();
    return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};
}

DocumentChange ReframeCommand::undo(Document& doc) {
    if (noop_) return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};

    doc.cmdSetCanvasSize(oldSize_);  // restore the canvas first
    shiftGeometry(doc, -dx_, -dy_);  // unshift geometry
    for (auto it = moves_.rbegin(); it != moves_.rend(); ++it) (*it)->undo(doc);  // unshift pixels
    doc.editableSelection() = oldSel_;  // restore the exact pre-reframe selection
    doc.touchSelection();
    return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};
}

std::int64_t ReframeCommand::retainedBytes() const noexcept {
    std::int64_t bytes = 0;
    for (const std::unique_ptr<PaintCommand>& m : moves_) {
        if (m) bytes += m->retainedBytes();
    }
    // The selection stores its tiles by value, so the snapshot is genuinely resident. Same
    // arithmetic SetSelectionCommand uses.
    bytes +=
        static_cast<std::int64_t>(oldSel_.tileCount()) * static_cast<std::int64_t>(kTilePixels);
    return bytes;
}

// ----------------------------------------------------------------- Crop

CropCommand::CropCommand(Rect cropRect) : crop_(cropRect) {}

bool CropCommand::plan(const Document& doc, Size& newSize, int& dx, int& dy) {
    const Rect eff = crop_.intersected(doc.canvasBounds());  // clamp to the canvas once
    if (eff.isEmpty()) return false;                         // degenerate crop: a no-op
    crop_ = eff;
    newSize = Size{eff.width, eff.height};
    dx = -eff.x;  // the cropped region lands at 0,0
    dy = -eff.y;
    return true;
}

// ---------------------------------------------------------- Canvas Size

Point canvasAnchorOffset(Size oldSize, Size newSize, CanvasAnchor anchor) noexcept {
    const int gw = newSize.width - oldSize.width;
    const int gh = newSize.height - oldSize.height;
    // Numerator over 2, so the three columns are 0, gw/2 and gw with one integer division and
    // no rounding drift between them. An odd growth puts the extra pixel on the right/bottom,
    // which is the convention every editor uses and which keeps a symmetric shrink symmetric.
    int nx = 0;
    int ny = 0;
    switch (anchor) {
        case CanvasAnchor::TopLeft:
            break;
        case CanvasAnchor::Top:
            nx = 1;
            break;
        case CanvasAnchor::TopRight:
            nx = 2;
            break;
        case CanvasAnchor::Left:
            ny = 1;
            break;
        case CanvasAnchor::Center:
            nx = 1;
            ny = 1;
            break;
        case CanvasAnchor::Right:
            nx = 2;
            ny = 1;
            break;
        case CanvasAnchor::BottomLeft:
            ny = 2;
            break;
        case CanvasAnchor::Bottom:
            nx = 1;
            ny = 2;
            break;
        case CanvasAnchor::BottomRight:
            nx = 2;
            ny = 2;
            break;
    }
    return Point{gw * nx / 2, gh * ny / 2};
}

CanvasResizeBlock canvasResizeBlocker(const Document& doc, Size newSize, CanvasAnchor anchor) {
    if (newSize.width < 1 || newSize.height < 1 || newSize.width > kMaxCanvasDimension ||
        newSize.height > kMaxCanvasDimension) {
        return CanvasResizeBlock::DimensionOutOfRange;
    }
    const Size oldSize = doc.canvasSize();
    if (newSize.width == oldSize.width && newSize.height == oldSize.height) {
        return CanvasResizeBlock::Unchanged;
    }
    const Point off = canvasAnchorOffset(oldSize, newSize, anchor);
    if (off.x == 0 && off.y == 0) return CanvasResizeBlock::None;  // nothing to move

    // Asked before the command is pushed, so a resize that cannot shift its content never
    // becomes a history entry that did nothing. moveRefusal answers the same question
    // moveLayerContent would, without building the command.
    std::vector<LayerId> pixelLayers;
    collectPixelLayers(doc.topLevelLayers(), pixelLayers);
    for (LayerId id : pixelLayers) {
        const Layer* l = doc.findLayer(id);
        if (l == nullptr || l->contentBounds().isEmpty()) continue;
        if (moveRefusal(doc, id, off.x, off.y).code != RefusalCode::None) {
            return CanvasResizeBlock::ContentNotShiftable;
        }
    }
    std::vector<LayerId> all;
    collectAllLayers(doc.topLevelLayers(), all);
    for (LayerId id : all) {
        const Layer* l = doc.findLayer(id);
        if (l == nullptr || l->mask() == nullptr || l->mask()->buffer().empty()) continue;
        if (!l->mask()->buffer().canTranslate(off.x, off.y, kMaxCropGeometryPixels)) {
            return CanvasResizeBlock::ContentNotShiftable;
        }
    }
    return CanvasResizeBlock::None;
}

ResizeCanvasCommand::ResizeCanvasCommand(Size newSize, CanvasAnchor anchor)
    : target_(newSize), anchor_(anchor) {}

bool ResizeCanvasCommand::plan(const Document& doc, Size& newSize, int& dx, int& dy) {
    if (canvasResizeBlocker(doc, target_, anchor_) != CanvasResizeBlock::None) return false;
    const Point off = canvasAnchorOffset(doc.canvasSize(), target_, anchor_);
    newSize = target_;
    dx = off.x;
    dy = off.y;
    return true;
}

}  // namespace pe
