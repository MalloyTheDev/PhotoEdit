#pragma once

#include "pe/core/BlendMode.hpp"
#include "pe/core/Color.hpp"
#include "pe/core/Command.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Tile.hpp"
#include "pe/core/TileStore.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pe {

class Document;

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
    struct Delta {
        TileCoord coord;
        std::shared_ptr<TileData> before;  // null == tile was absent (transparent)
        std::shared_ptr<TileData> after;   // the painted tile
    };

    PaintCommand(LayerId layer, Rect dirtyRect, std::vector<Delta> deltas, std::string name);

    [[nodiscard]] std::string name() const override { return name_; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

    [[nodiscard]] std::size_t touchedTileCount() const noexcept { return deltas_.size(); }

private:
    LayerId layer_;
    Rect dirty_;
    std::vector<Delta> deltas_;
    std::string name_;
};

// Build a PaintCommand depositing `points` with `settings` and `color` into the
// pixel layer `layerId`. Returns nullptr if the layer is not a pixel layer or the
// stroke deposits nothing. The command is NOT applied yet — push it to history.
[[nodiscard]] std::unique_ptr<PaintCommand> paintStroke(Document& doc, LayerId layerId,
                                                        const BrushSettings& settings, Rgbaf color,
                                                        std::span<const StrokePoint> points);

// Eraser: the same pipeline, but it reduces alpha (paints transparency) instead
// of depositing color.
[[nodiscard]] std::unique_ptr<PaintCommand> eraseStroke(Document& doc, LayerId layerId,
                                                        const BrushSettings& settings,
                                                        std::span<const StrokePoint> points);

}  // namespace pe
