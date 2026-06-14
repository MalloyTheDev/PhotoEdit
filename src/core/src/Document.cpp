#include "pe/core/Document.hpp"

#include "pe/core/Compositor.hpp"
#include "pe/core/PixelLayer.hpp"

#include <algorithm>

namespace pe {

Document::Document(Size canvasSize, ColorMode mode, BitDepth depth, int ppi)
    : canvasSize_(canvasSize),
      resolutionPpi_(ppi),
      colorMode_(mode),
      bitDepth_(depth),
      history_(*this) {}

std::unique_ptr<Document> Document::createBlank(Size canvasSize, ColorMode mode, BitDepth depth,
                                                int ppi) {
    // Validate: positive, in-range dimensions and a positive resolution.
    if (canvasSize.width < 1 || canvasSize.height < 1) return nullptr;
    if (canvasSize.width > kMaxCanvasDimension || canvasSize.height > kMaxCanvasDimension) {
        return nullptr;
    }
    if (ppi <= 0) return nullptr;

    // private ctor -> wrap in unique_ptr (can't use make_unique with a private ctor).
    std::unique_ptr<Document> doc(new Document(canvasSize, mode, depth, ppi));

    // Seed one transparent base layer; no tiles are allocated until something is
    // painted, so even a huge canvas costs nothing here.
    auto base = std::make_unique<PixelLayer>("Background");
    const LayerId baseId = base->id();
    doc->root_.addChild(std::move(base));
    doc->activeLayer_ = baseId;
    doc->dirty_ = false;
    return doc;
}

const Layer* Document::findLayer(LayerId id) const noexcept {
    if (id == kNoLayer) return nullptr;
    return root_.findDescendant(id);
}

Layer* Document::findLayer(LayerId id) noexcept {
    if (id == kNoLayer) return nullptr;
    return root_.findDescendant(id);
}

PixelBuffer Document::compositeImage() const {
    return compositeToImage(root_.children(), canvasBounds());
}

void Document::addObserver(DocumentObserver* obs) {
    if (obs == nullptr) return;
    if (std::find(observers_.begin(), observers_.end(), obs) == observers_.end()) {
        observers_.push_back(obs);
    }
}

void Document::removeObserver(DocumentObserver* obs) {
    observers_.erase(std::remove(observers_.begin(), observers_.end(), obs), observers_.end());
}

void Document::cmdInsertTopLevel(std::size_t index, std::unique_ptr<Layer> layer) {
    root_.insertChild(index, std::move(layer));
}

std::unique_ptr<Layer> Document::cmdRemoveTopLevel(LayerId id) {
    auto removed = root_.removeChild(id);
    // If the active layer was removed, clear it (a command may reassign).
    if (removed && activeLayer_ == id) activeLayer_ = kNoLayer;
    return removed;
}

std::size_t Document::topLevelIndexOf(LayerId id) const noexcept {
    return root_.indexOf(id);
}

void Document::setActiveLayer(LayerId id) {
    if (activeLayer_ == id) return;
    // Only accept ids that exist (or kNoLayer to clear).
    if (id != kNoLayer && findLayer(id) == nullptr) return;
    activeLayer_ = id;
    notify(DocumentChange{DocumentChange::Kind::ActiveLayer, Rect{}, id});
}

void Document::notify(const DocumentChange& change) {
    // Iterate a snapshot so an observer may add/remove observers during dispatch.
    const std::vector<DocumentObserver*> snapshot = observers_;
    for (DocumentObserver* obs : snapshot) {
        if (obs != nullptr) obs->onDocumentChanged(*this, change);
    }
}

void Document::setDirty(bool dirty) {
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    notify(DocumentChange{DocumentChange::Kind::DirtyState, Rect{}, kNoLayer});
}

}  // namespace pe
