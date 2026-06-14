#include "pe/core/Commands.hpp"

#include "pe/core/Document.hpp"

#include <utility>

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
    // If we just removed the active layer, clear the selection (and notify).
    if (prevActive_ == layerId_) doc.setActiveLayer(kNoLayer);
    Rect region;
    if (owned_) region = owned_->contentBounds();
    return structureChange(region, layerId_);
}

DocumentChange RemoveLayerCommand::undo(Document& doc) {
    Rect region;
    if (owned_) region = owned_->contentBounds();
    doc.cmdInsertTopLevel(index_, std::move(owned_));
    // Restore the active layer if this command had cleared it.
    if (prevActive_ == layerId_) doc.setActiveLayer(layerId_);
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

}  // namespace pe
