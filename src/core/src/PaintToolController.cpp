#include "pe/core/PaintToolController.hpp"

#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/History.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pe {

namespace {
// Foreground luminance that drives mask painting: 0 (black) hides, 1 (white) reveals. Rec.601
// weights, matching the engine's other luma uses; clamped because an HDR/F32 color may exceed 1.
// The foreground ALPHA is intentionally ignored — a mask stores single-channel coverage, so only
// the color's luminance is meaningful (a transparent foreground still deposits its RGB-derived
// gray).
[[nodiscard]] float maskGrayFromColor(Rgbaf c) noexcept {
    return clamp01(0.299f * c.r + 0.587f * c.g + 0.114f * c.b);
}
}  // namespace

PaintToolController::~PaintToolController() = default;

std::unique_ptr<Command> PaintToolController::buildStroke(Document& doc) const {
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
        case Mode::MaskPaint:
            // Paint the active layer's mask toward the foreground luminance (black hides, white
            // reveals) instead of depositing color into the pixels.
            return maskPaintStroke(doc, layer_, brush_, points_, maskGrayFromColor(color_),
                                   selection_);
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
    if (active == nullptr) return false;
    if (mode_ == Mode::MaskPaint) {
        // Mask painting targets the active layer's MASK, so any layer kind that carries one is
        // paintable (e.g. an adjustment or solid-color layer with a mask) — but a mask must exist.
        if (active->mask() == nullptr) return false;
    } else if (active->kind() != LayerKind::Pixel) {
        return false;
    }

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
    std::unique_ptr<Command> cmd;
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
