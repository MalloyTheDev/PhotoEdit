#pragma once

#include "pe/core/Adjustment.hpp"
#include "pe/core/Command.hpp"
#include "pe/core/Layer.hpp"

#include <memory>

namespace pe {

class Document;

// A Layer that owns an Adjustment (its parameters) and transforms the accumulated
// composite beneath it at render time — non-destructive color/tonal editing. It
// contributes no pixels; the compositor invokes applyTo() on the backdrop, then
// blends the adjusted copy back via this layer's blend mode / opacity / mask.
// See docs/systems/05-adjustment-layers.md.
class AdjustmentLayer final : public Layer {
public:
    AdjustmentLayer(std::unique_ptr<Adjustment> adjustment, std::string name);

    [[nodiscard]] const Adjustment& adjustment() const noexcept { return *adjustment_; }
    void setAdjustment(std::unique_ptr<Adjustment> a) noexcept {
        if (a) adjustment_ = std::move(a);
    }
    // Swap the adjustment with another (the undoable EditAdjustmentCommand uses this).
    void swapAdjustment(std::unique_ptr<Adjustment>& other) noexcept { adjustment_.swap(other); }

    [[nodiscard]] bool isAdjustment() const noexcept override { return true; }
    void applyTo(std::span<Rgbaf> backdrop, TileCoord coord) const override;

    [[nodiscard]] Rect contentBounds() const noexcept override;
    void renderInto(TileCoord coord, std::span<Rgbaf> dst) const override;  // no pixels
    [[nodiscard]] std::unique_ptr<Layer> clone() const override;

private:
    std::unique_ptr<Adjustment> adjustment_;
};

// Reversible edit of an adjustment layer's parameters: swaps in a new Adjustment
// and restores the old on undo. Undo is a tiny parameter delta (no pixels), and
// recompositing is bounded to the tiles the adjustment covers.
class EditAdjustmentCommand final : public Command {
public:
    EditAdjustmentCommand(LayerId layer, std::unique_ptr<Adjustment> newAdjustment);
    [[nodiscard]] std::string name() const override { return "Edit Adjustment"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    DocumentChange swap(Document&);
    LayerId layer_;
    std::unique_ptr<Adjustment> pending_;  // holds the "other" adjustment between swaps
};

}  // namespace pe
