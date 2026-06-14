#include "pe/core/AdjustmentLayer.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"  // bakePixelEdit

namespace pe {

std::unique_ptr<PaintCommand> applyAdjustment(Document& doc, LayerId layerId,
                                              const Adjustment& adjustment,
                                              const Selection* selection) {
    return bakePixelEdit(
        doc, layerId, adjustment.name(),
        [&](std::span<Rgbaf> img, int, int) { adjustment.apply(img); }, selection);
}

namespace {
// An adjustment layer affects "everything beneath it"; report a large bounds so it
// is never culled and an edit invalidates the whole composite below it (the renderer
// collapses an over-threshold invalidate region into a full recomposite). Sized so
// tilesForRect(kEverywhere).count() stays within int (no overflow) while still
// covering any realistic canvas (max dimension 300000).
constexpr Rect kEverywhere{-2'000'000, -2'000'000, 4'000'000, 4'000'000};
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
