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
            int offX = 0;
            int offY = 0;
            if (!cloneOffset(offX, offY)) return nullptr;  // no source anchor set / bad first point
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

bool PaintToolController::cloneOffset(int& offX, int& offY) const {
    if (!cloneSourceValid_ || points_.empty()) return false;  // no source anchor set
    // Lock the source->dest offset to the stroke's first point. Validate + clamp the (untrusted,
    // possibly tablet-garbage) first point before the float->int conversion: a non-finite or huge
    // value would make lround out-of-range (float->int UB) and overflow the offset subtraction.
    const double fx = points_[0].pos.x;
    const double fy = points_[0].pos.y;
    if (!std::isfinite(fx) || !std::isfinite(fy)) return false;
    constexpr double kBound = static_cast<double>(kMaxCanvasDimension);
    const int px = static_cast<int>(std::lround(std::clamp(fx, -kBound, kBound)));
    const int py = static_cast<int>(std::lround(std::clamp(fy, -kBound, kBound)));
    // Clamp the anchor to the canvas range too (setCloneSource doesn't), so the offset subtraction
    // can't signed-overflow even via a direct caller: both operands are within
    // +/-kMaxCanvasDimension.
    const int csx = std::clamp(cloneSource_.x, -kMaxCanvasDimension, kMaxCanvasDimension);
    const int csy = std::clamp(cloneSource_.y, -kMaxCanvasDimension, kMaxCanvasDimension);
    offX = px - csx;
    offY = py - csy;
    return true;
}

std::unique_ptr<LiveStroke> PaintToolController::createLive(Document& doc) {
    // Stabilization smooths the whole path, which the incremental stamper doesn't model — fall back
    // to the batched rebuild when it is on.
    if (brush_.stabilize > 0.0f) return nullptr;
    switch (mode_) {
        case Mode::Brush:
            return beginPaintStroke(doc, layer_, brush_, color_, selection_);
        case Mode::Eraser:
            return beginEraseStroke(doc, layer_, brush_, selection_);
        case Mode::Dodge:
            return beginDodgeStroke(doc, layer_, brush_, selection_);
        case Mode::Burn:
            return beginBurnStroke(doc, layer_, brush_, selection_);
        case Mode::Clone: {
            int offX = 0;
            int offY = 0;
            if (!cloneOffset(offX, offY)) return nullptr;  // no source -> nothing to clone
            return beginCloneStroke(doc, layer_, brush_, offX, offY, selection_);
        }
        default:
            return nullptr;  // Blur/Sharpen/Heal (region bake) and MaskPaint use the batched path
    }
}

bool PaintToolController::liveTargetValid(Document& doc) const {
    // The live path is only ever used for pixel-layer ops (createLive returns nullptr for MaskPaint
    // and the region-bake brushes), so a still-paintable target is a present Pixel layer.
    const Layer* l = doc.findLayer(layer_);
    return l != nullptr && l->kind() == LayerKind::Pixel;
}

void PaintToolController::resetStroke() noexcept {
    stroking_ = false;
    points_.clear();
    layer_ = kNoLayer;
    selection_ = nullptr;
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
    // The per-pixel ops use the incremental LiveStroke (linear over the stroke); everything else
    // (region-bake brushes, mask paint, stabilized strokes) keeps the batched rebuild path.
    live_ = createLive(doc);
    if (live_) {
        strokeDirty_ = strokeDirty_.united(live_->extend(points_));
    } else {
        rebuildPreview(doc);
    }
    return true;
}

void PaintToolController::extend(Document& doc, StrokePoint p) {
    if (!stroking_) return;
    if (live_ && !liveTargetValid(doc)) {
        // The target layer was removed/replaced between samples (a contract violation). The live
        // stroke borrows that layer's tile store by reference, so touching it now would be a
        // use-after-free; drop the stroke instead (the LiveStroke destructor frees only its own
        // buffers). This matches the batched path, which re-resolves by id and degrades to a no-op.
        live_.reset();
        resetStroke();
        return;
    }
    points_.push_back(p);
    if (live_) {
        // Incremental: stamp only the new dabs and recomposite just the tiles they touched.
        strokeDirty_ = strokeDirty_.united(live_->extend(points_));
    } else {
        // Batched: revert the prior preview so the rebuild sees the original tiles, then recompute
        // the whole stroke (the region-bake/mask engines resample from scratch).
        clearPreview(doc);
        rebuildPreview(doc);
    }
}

bool PaintToolController::end(Document& doc) {
    if (!stroking_) return false;
    stroking_ = false;

    std::unique_ptr<Command> cmd;
    if (live_) {
        // The layer already holds the final pixels; finish() yields the byte-exact command (its
        // before == the pre-stroke snapshots). Pushing it re-applies a no-op and notifies
        // observers. If the target layer vanished mid-stroke, drop the stroke without dereferencing
        // its now-dangling store (commits nothing).
        if (liveTargetValid(doc)) cmd = live_->finish();
        live_.reset();
    } else if (preview_) {
        // Reuse the preview as the committed command: revert it (tiles back to S0) and hand it to
        // History, which re-executes it from S0 — exactly one undo step.
        preview_->undo(doc);
        cmd = std::move(preview_);
    }
    resetStroke();

    if (cmd == nullptr) return false;  // stroke deposited nothing
    doc.history().push(std::move(cmd));
    return true;
}

void PaintToolController::cancel(Document& doc) {
    if (!stroking_) return;
    if (live_) {
        // Restore every touched tile to its pre-stroke snapshot — but only if the target layer is
        // still here; if it vanished mid-stroke its store reference dangles, so just drop the
        // stroke.
        if (liveTargetValid(doc)) live_->cancel();
        live_.reset();
    } else {
        clearPreview(doc);
    }
    resetStroke();
}

}  // namespace pe
