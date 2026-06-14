#include "pe/core/AdjustmentLayer.hpp"

#include "pe/core/Document.hpp"

namespace pe {

namespace {
// An adjustment layer affects "everything beneath it"; report a very large bounds
// so it is never culled and an edit invalidates the whole composite below it (the
// renderer collapses an over-threshold invalidate region into a full recomposite).
constexpr Rect kEverywhere{-(1 << 24), -(1 << 24), 1 << 25, 1 << 25};
}  // namespace

AdjustmentLayer::AdjustmentLayer(std::unique_ptr<Adjustment> adjustment, std::string name)
    : Layer(LayerKind::Adjustment, std::move(name)), adjustment_(std::move(adjustment)) {}

void AdjustmentLayer::applyTo(std::span<Rgbaf> backdrop, TileCoord /*coord*/) const {
    if (adjustment_) adjustment_->apply(backdrop);
}

Rect AdjustmentLayer::contentBounds() const noexcept {
    return kEverywhere;
}

void AdjustmentLayer::renderInto(TileCoord /*coord*/, std::span<Rgbaf> dst) const {
    // Adjustment layers contribute no pixels; the compositor calls applyTo() and
    // never renderInto(). Implemented for the Layer contract: yield transparent.
    for (Rgbaf& p : dst) p = Rgbaf{};
}

std::unique_ptr<Layer> AdjustmentLayer::clone() const {
    auto copy =
        std::make_unique<AdjustmentLayer>(adjustment_ ? adjustment_->clone() : nullptr, name());
    copyPropsTo(*copy);
    return copy;
}

// ---------------------------------------------------------------- EditAdjustment

EditAdjustmentCommand::EditAdjustmentCommand(LayerId layer,
                                             std::unique_ptr<Adjustment> newAdjustment)
    : layer_(layer), pending_(std::move(newAdjustment)) {}

DocumentChange EditAdjustmentCommand::swap(Document& doc) {
    Layer* layer = doc.findLayer(layer_);
    if (layer != nullptr && layer->isAdjustment() && pending_ != nullptr) {
        static_cast<AdjustmentLayer*>(layer)->swapAdjustment(pending_);
    }
    return DocumentChange{DocumentChange::Kind::LayerProps,
                          layer != nullptr ? layer->contentBounds() : Rect{}, layer_};
}

DocumentChange EditAdjustmentCommand::execute(Document& doc) {
    return swap(doc);
}
DocumentChange EditAdjustmentCommand::undo(Document& doc) {
    return swap(doc);
}

}  // namespace pe
