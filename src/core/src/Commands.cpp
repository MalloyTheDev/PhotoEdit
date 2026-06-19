#include "pe/core/Commands.hpp"

#include "pe/core/Brush.hpp"  // PaintCommand (CropCommand composes per-layer moves)
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"      // moveLayerContent
#include "pe/core/GroupLayer.hpp"  // recurse into groups for crop
#include "pe/core/PixelLayer.hpp"  // contentBounds() on the concrete pixel layer

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
        oldSel_ = doc.selection();  // capture the prior selection once, for undo
        captured_ = true;
    }
    doc.editableSelection() = newSel_;
    doc.touchSelection();
    return DocumentChange{DocumentChange::Kind::Selection, Rect{}, kNoLayer};
}

DocumentChange SetSelectionCommand::undo(Document& doc) {
    doc.editableSelection() = oldSel_;
    doc.touchSelection();
    return DocumentChange{DocumentChange::Kind::Selection, Rect{}, kNoLayer};
}

// ----------------------------------------------------------------- Crop

CropCommand::CropCommand(Rect cropRect) : crop_(cropRect) {}
CropCommand::~CropCommand() = default;

DocumentChange CropCommand::execute(Document& doc) {
    if (!captured_) {
        captured_ = true;
        oldSize_ = doc.canvasSize();
        oldSel_ = doc.selection();  // snapshot for undo (once); shifted on every execute below
        const Rect eff = crop_.intersected(doc.canvasBounds());  // clamp to the canvas once
        if (eff.isEmpty()) {
            crop_ = Rect{};  // degenerate crop: the command is a no-op
        } else {
            crop_ = eff;
            const bool needShift = eff.x != 0 || eff.y != 0;
            std::vector<LayerId> pixelLayers;
            collectPixelLayers(doc.topLevelLayers(), pixelLayers);  // incl. nested-in-group
            // Build (but don't yet apply) a content shift for every pixel layer, so each move
            // snapshots the ORIGINAL pixels. A zero shift / empty layer yields null.
            bool shiftable = true;
            for (LayerId id : pixelLayers) {
                if (auto m = moveLayerContent(doc, id, -eff.x, -eff.y)) {
                    moves_.push_back(std::move(m));
                } else if (needShift) {
                    // A null with a real shift means either an empty layer (fine to skip) or a
                    // layer whose content exceeds the move budget. The latter would leave the
                    // doc half-cropped (resized but content not moved), so refuse the crop.
                    const Layer* l = doc.findLayer(id);
                    if (l != nullptr &&
                        !static_cast<const PixelLayer*>(l)->contentBounds().isEmpty()) {
                        shiftable = false;
                        break;
                    }
                }
            }
            if (!shiftable) {
                moves_.clear();  // none executed yet; abort to a no-op rather than half-crop
                crop_ = Rect{};
            }
        }
    }
    if (crop_.isEmpty())
        return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};

    for (auto& m : moves_) m->execute(doc);
    doc.cmdSetCanvasSize(Size{crop_.width, crop_.height});
    // Shift the active selection by the same -origin so it tracks the cropped content. Recomputed
    // from the captured original each time, so redo is exact (re-shifting an inactive sel is a
    // no-op). Notify observers (marching ants) just as SetSelectionCommand does.
    doc.editableSelection() = translatedSelection(oldSel_, -crop_.x, -crop_.y);
    doc.touchSelection();
    return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};
}

DocumentChange CropCommand::undo(Document& doc) {
    if (crop_.isEmpty())
        return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};

    doc.cmdSetCanvasSize(oldSize_);  // restore the canvas first
    for (auto it = moves_.rbegin(); it != moves_.rend(); ++it) (*it)->undo(doc);  // unshift
    doc.editableSelection() = oldSel_;  // restore the exact pre-crop selection
    doc.touchSelection();
    return DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer};
}

}  // namespace pe
