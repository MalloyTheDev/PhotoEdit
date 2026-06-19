#include "pe/core/PaintToolController.hpp"

#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/History.hpp"

#include <algorithm>
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
        case Mode::Blur:
            return blurStroke(doc, layer_, brush_, points_, selection_);
        case Mode::Sharpen:
            return sharpenStroke(doc, layer_, brush_, points_, selection_);
        case Mode::Heal:
            return healStroke(doc, layer_, brush_, points_, selection_);
        case Mode::Clone: {
            if (!cloneSourceValid_ || points_.empty()) return nullptr;  // no source anchor set
            // Lock the source->dest offset to the stroke's first point (sampled every rebuild from
            // points_[0], so it is stable across extend()). Validate + clamp the (untrusted,
            // possibly tablet-garbage) first point before the float->int conversion: a non-finite
            // or huge value would make lround out-of-range (float->int UB) and overflow the offset
            // subtraction.
            const double fx = points_[0].pos.x;
            const double fy = points_[0].pos.y;
            if (!std::isfinite(fx) || !std::isfinite(fy)) return nullptr;
            constexpr double kBound = static_cast<double>(kMaxCanvasDimension);
            const int px = static_cast<int>(std::lround(std::clamp(fx, -kBound, kBound)));
            const int py = static_cast<int>(std::lround(std::clamp(fy, -kBound, kBound)));
            // Clamp the anchor to the canvas range too (setCloneSource doesn't), so the offset
            // subtraction can't signed-overflow even via a direct, non-UI caller: both operands are
            // now within +/-kMaxCanvasDimension, so the difference fits comfortably in int.
            const int csx = std::clamp(cloneSource_.x, -kMaxCanvasDimension, kMaxCanvasDimension);
            const int csy = std::clamp(cloneSource_.y, -kMaxCanvasDimension, kMaxCanvasDimension);
            const int offX = px - csx;
            const int offY = py - csy;
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
