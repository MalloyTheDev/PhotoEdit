#pragma once

#include "pe/core/BlendMode.hpp"
#include "pe/core/Color.hpp"
#include "pe/core/Command.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelFormat.hpp"
#include "pe/core/Tile.hpp"
#include "pe/core/TileStore.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pe {

class Document;
class Selection;

// A sub-pixel position (brush sampling lives between pixel centers).
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// One input sample along a stroke (mouse, tablet, or programmatic).
struct StrokePoint {
    Vec2 pos;               // document-space, sub-pixel
    float pressure = 1.0f;  // [0,1]; 1.0 for devices without pressure
};

// The M3 brush definition: a round tip with hardness, the opacity/flow pair, and
// spacing. Dynamics, sampled tips, texture, dual brush, and color dynamics are
// modeled in the spec (docs/systems/08-brush-engine.md) and arrive in later
// milestones; this is the core that proves the pipeline and undo.
struct BrushSettings {
    float diameter = 20.0f;  // px at pressure == 1
    float hardness = 1.0f;   // [0,1]; 1 = crisp, 0 = soft shoulder
    float opacity = 1.0f;    // [0,1] STROKE ceiling
    float flow = 1.0f;       // [0,1] per-dab deposition
    float spacing = 0.25f;   // dab spacing as a fraction of diameter
    BlendMode blendMode = BlendMode::Normal;
    bool pressureControlsSize = false;  // scale diameter by pressure if true
    float stabilize = 0.0f;             // [0,1] low-pass filter strength for stabilization
};

// Radial dab coverage in [0,1]: 1 at/under the hardness radius, a smoothstep
// shoulder out to 0 at the rim (d >= 1). `d` is the normalized distance from the
// dab center. See docs/systems/08-brush-engine.md.
[[nodiscard]] float dabCoverage(float d, float hardness) noexcept;

// A reversible paint stroke, recorded as a tile delta: for each touched tile it
// keeps the prior tile snapshot (shared, copy-on-write) and the painted result,
// so undo restores byte-exact prior pixels and undo memory scales with painted
// area (ADR-0005, ADR-0003).
class PaintCommand final : public Command {
public:
    // One tile's before/after snapshot at a given storage depth. before == null
    // means the tile was absent (transparent). Tiles are shared copy-on-write.
    template <class Pixel>
    struct DeltaT {
        TileCoord coord;
        std::shared_ptr<TileDataT<Pixel>> before;
        std::shared_ptr<TileDataT<Pixel>> after;
    };
    using Delta = DeltaT<Rgba8>;     // 8-bit (the default storage depth)
    using Delta16 = DeltaT<Rgba16>;  // 16-bit
    using DeltaF = DeltaT<Rgbaf>;    // 32-bit float

    // One constructor per depth; the command applies/reverts into the layer's store
    // of the matching depth. The 8-bit overload is the original signature.
    PaintCommand(LayerId layer, Rect dirtyRect, std::vector<Delta> deltas, std::string name);
    PaintCommand(LayerId layer, Rect dirtyRect, std::vector<Delta16> deltas, std::string name);
    PaintCommand(LayerId layer, Rect dirtyRect, std::vector<DeltaF> deltas, std::string name);

    [[nodiscard]] std::string name() const override { return name_; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

    [[nodiscard]] std::size_t touchedTileCount() const noexcept;

private:
    // Shared execute/undo body: applies the active-depth deltas' `after` (forward)
    // or `before` (reverse) snapshots into the layer's matching store.
    DocumentChange apply(Document& doc, bool forward);

    LayerId layer_;
    Rect dirty_;
    BitDepth depth_ = BitDepth::U8;
    std::vector<Delta> deltas8_;
    std::vector<Delta16> deltas16_;
    std::vector<DeltaF> deltasF_;
    std::string name_;
};

// Build a PaintCommand depositing `points` with `settings` and `color` into the
// pixel layer `layerId`. If `selection` is non-null and active, painting is gated
// (coverage multiplied) by it, so a brush is confined to the selected region with
// soft edges for free. Returns nullptr if the layer is not a pixel layer or the
// stroke deposits nothing. The command is NOT applied yet — push it to history.
[[nodiscard]] std::unique_ptr<PaintCommand> paintStroke(Document& doc, LayerId layerId,
                                                        const BrushSettings& settings, Rgbaf color,
                                                        std::span<const StrokePoint> points,
                                                        const Selection* selection = nullptr);

// Eraser: the same pipeline, but it reduces alpha (paints transparency) instead
// of depositing color.
[[nodiscard]] std::unique_ptr<PaintCommand> eraseStroke(Document& doc, LayerId layerId,
                                                        const BrushSettings& settings,
                                                        std::span<const StrokePoint> points,
                                                        const Selection* selection = nullptr);

// Dodge (lighten) / Burn (darken): the same brush pipeline, but each dab adjusts the tone of the
// EXISTING pixels under the brush instead of depositing color — weighted by brush coverage (so
// size/hardness/flow/opacity and the selection all apply). Alpha is preserved and fully
// transparent pixels are left untouched. Returns nullptr if not a pixel layer or nothing changes.
[[nodiscard]] std::unique_ptr<PaintCommand> dodgeStroke(Document& doc, LayerId layerId,
                                                        const BrushSettings& settings,
                                                        std::span<const StrokePoint> points,
                                                        const Selection* selection = nullptr);
[[nodiscard]] std::unique_ptr<PaintCommand> burnStroke(Document& doc, LayerId layerId,
                                                       const BrushSettings& settings,
                                                       std::span<const StrokePoint> points,
                                                       const Selection* selection = nullptr);

// Clone Stamp: the same brush pipeline, but each painted pixel (x, y) is composited from the
// layer's PRE-STROKE pixel at (x - offsetX, y - offsetY) — `offset` is the first stroke point minus
// the user's clone-source anchor. Sampling the pre-stroke state means source/dest overlap does not
// feed back. Honors the selection. Returns nullptr if not a pixel layer or nothing is deposited
// (e.g. the source region is empty/transparent).
[[nodiscard]] std::unique_ptr<PaintCommand> cloneStroke(Document& doc, LayerId layerId,
                                                        const BrushSettings& settings,
                                                        std::span<const StrokePoint> points,
                                                        int offsetX, int offsetY,
                                                        const Selection* selection = nullptr);

// Blur brush: locally blurs the EXISTING pixels under the stroke, weighted by brush coverage.
// Unlike the per-pixel ops above, blurring needs a neighborhood, so this runs ONE region bake over
// the stroke's bounding box (the same reversible tile-delta machinery as destructive filters): the
// region is gaussian-blurred once and each pixel is lerped from its original toward its blurred
// value by its accumulated brush coverage (capped at the stroke opacity). Honors the selection
// (effective strength is brushCoverage * selectionCoverage). Returns nullptr if not a pixel layer,
// the stroke has no coverage, or the bounding region is over the engine's per-op budget.
[[nodiscard]] std::unique_ptr<PaintCommand> blurStroke(Document& doc, LayerId layerId,
                                                       const BrushSettings& settings,
                                                       std::span<const StrokePoint> points,
                                                       const Selection* selection = nullptr);

}  // namespace pe
