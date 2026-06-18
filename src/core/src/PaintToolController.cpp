#include "pe/core/PaintToolController.hpp"

#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/History.hpp"

#include <cmath>
#include <utility>

namespace pe {

PaintToolController::~PaintToolController() = default;

std::unique_ptr<PaintCommand> PaintToolController::buildStroke(Document& doc) const {
    switch (mode_) {
        case Mode::Eraser:
            return eraseStroke(doc, layer_, brush_, points_, selection_);
        case Mode::Dodge:
            return dodgeStroke(doc, layer_, brush_, points_, selection_);
        case Mode::Burn:
            return burnStroke(doc, layer_, brush_, points_, selection_);
        case Mode::Clone: {
            if (!cloneSourceValid_ || points_.empty()) return nullptr;  // no source anchor set
            // Lock the source->dest offset to the stroke's first point (sampled every rebuild from
            // points_[0], so it is stable across extend()).
            const int offX = static_cast<int>(std::lround(points_[0].pos.x)) - cloneSource_.x;
            const int offY = static_cast<int>(std::lround(points_[0].pos.y)) - cloneSource_.y;
            return cloneStroke(doc, layer_, brush_, points_, offX, offY, selection_);
        }
        case Mode::Brush:
        default:
            return paintStroke(doc, layer_, brush_, color_, points_, selection_);
    }
}

void PaintToolController::clearPreview(Document& doc) {
    if (preview_) {
        preview_->undo(doc);  // restore the touched tiles to their pre-stroke state
        preview_.reset();
    }
}

void PaintToolController::rebuildPreview(Document& doc) {
    // Precondition: the layer's tiles are at their pre-stroke (S0) state, so the
    // command captured here records before == S0 for every touched tile.
    preview_ = buildStroke(doc);
    if (preview_) {
        const DocumentChange ch = preview_->execute(doc);
        // Accumulate the touched region so a view can repaint just the stroke's
        // footprint instead of recompositing the whole canvas each sample.
        strokeDirty_ = strokeDirty_.united(ch.dirtyRegion);
    }
}

bool PaintToolController::begin(Document& doc, StrokePoint p, const Selection* selection) {
    if (stroking_) return false;
    const Layer* active = doc.findLayer(doc.activeLayer());
    if (active == nullptr || active->kind() != LayerKind::Pixel) return false;

    stroking_ = true;
    layer_ = doc.activeLayer();
    selection_ = selection;
    points_.clear();
    points_.push_back(p);
    strokeDirty_ = Rect{};  // fresh stroke: start the dirty-bounds accumulator empty
    rebuildPreview(doc);
    return true;
}

void PaintToolController::extend(Document& doc, StrokePoint p) {
    if (!stroking_) return;
    // Revert the prior preview so the rebuild sees the original tiles again, then
    // recompute the whole stroke (the brush engine resamples dabs along the path).
    clearPreview(doc);
    points_.push_back(p);
    rebuildPreview(doc);
}

bool PaintToolController::end(Document& doc) {
    if (!stroking_) return false;
    stroking_ = false;

    // Reuse the preview as the committed command: revert it (tiles back to S0) and
    // hand it to History, which re-executes it from S0 — exactly one undo step.
    std::unique_ptr<PaintCommand> cmd;
    if (preview_) {
        preview_->undo(doc);
        cmd = std::move(preview_);
    }
    points_.clear();
    layer_ = kNoLayer;
    selection_ = nullptr;

    if (cmd == nullptr) return false;  // stroke deposited nothing
    doc.history().push(std::move(cmd));
    return true;
}

void PaintToolController::cancel(Document& doc) {
    if (!stroking_) return;
    clearPreview(doc);
    stroking_ = false;
    points_.clear();
    layer_ = kNoLayer;
    selection_ = nullptr;
}

}  // namespace pe
