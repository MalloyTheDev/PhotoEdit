#include "pe/core/Brush.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace pe {

float dabCoverage(float d, float hardness) noexcept {
    const float h = clamp01(hardness);
    if (d >= 1.0f) return 0.0f;
    if (d <= h) return 1.0f;
    // Smoothstep shoulder from the hardness radius (coverage 1) to the rim (0).
    const float t = (d - h) / (1.0f - h);  // 0..1 across the soft band; h<1 here
    const float s = 1.0f - t;
    return s * s * (3.0f - 2.0f * s);  // smoothstep(s)
}

namespace {

enum class PaintOp { Paint, Erase, Dodge, Burn, Clone };

// Per-stroke tone strength for Dodge/Burn at full brush coverage, modulated further by the brush
// opacity/flow. Kept low so each pass is a modest adjustment that builds up over repeated strokes
// (a photographic dodge/burn "exposure" feel) rather than slamming pixels to white/black.
constexpr float kToneExposure = 0.3f;

using CoverageKey = std::pair<int, int>;  // {tileCol, tileRow}
using CoverageMap = std::map<CoverageKey, std::vector<float>>;

// Guard against a degenerate spacing/length combination producing a runaway dab
// count (it cannot hang, but bound it defensively).
constexpr int64_t kMaxDabsPerStroke = 2'000'000;
// Largest brush diameter we honor (caps a single dab's bbox).
constexpr float kMaxBrushDiameter = 5000.0f;
// Hard ceiling on stroke-buffer tiles, so a pathological stroke cannot exhaust
// memory; reached only by degenerate input and degrades gracefully (the stroke
// is clipped, never a crash/OOM).
constexpr std::size_t kMaxStrokeTiles = 4096;
// Coordinate magnitude beyond which a dab is treated as absurdly off-canvas and
// skipped. Keeps center +/- radius and col*kTileSize comfortably within int.
constexpr float kCoordBound = static_cast<float>(1 << 26);  // ~67M

void stampDab(CoverageMap& cov, Vec2 center, float radius, float hardness, float flow) {
    // C1: reject non-finite/absurd coordinates before the float->int casts (which
    // are UB for NaN/Inf/out-of-range values).
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius)) return;
    if (std::fabs(center.x) > kCoordBound || std::fabs(center.y) > kCoordBound) return;

    const int x0 = static_cast<int>(std::floor(center.x - radius));
    const int x1 = static_cast<int>(std::ceil(center.x + radius));
    const int y0 = static_cast<int>(std::floor(center.y - radius));
    const int y1 = static_cast<int>(std::ceil(center.y + radius));
    const float invR = 1.0f / radius;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f - center.x) * invR;
            const float dy = (static_cast<float>(y) + 0.5f - center.y) * invR;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float c = dabCoverage(d, hardness) * flow;
            if (c <= 0.0f) continue;

            const int col = floorDiv(x, kTileSize);
            const int row = floorDiv(y, kTileSize);
            const CoverageKey key{col, row};
            auto it = cov.find(key);
            if (it == cov.end()) {
                // C2: bound total stroke-buffer memory; stop adding tiles past the cap.
                if (cov.size() >= kMaxStrokeTiles) continue;
                it = cov.emplace(key,
                                 std::vector<float>(static_cast<std::size_t>(kTilePixels), 0.0f))
                         .first;
            }
            std::vector<float>& buf = it->second;

            const int lx = x - col * kTileSize;
            const int ly = y - row * kTileSize;
            float& b = buf[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)];
            b = b + c * (1.0f - b);  // Porter-Duff 'over' on stroke coverage (caps at 1)
        }
    }
}

// Composite the accumulated stroke coverage into `store` at its native depth: for
// each touched tile, blend the dab color (or erase alpha) over the prior pixels in
// float and write back native-depth tile deltas. One template per pixel type keeps
// the dab/composite math identical across depths.
template <class Pixel>
std::unique_ptr<PaintCommand> flushStroke(LayerId layerId, TileStoreT<Pixel>& store,
                                          const CoverageMap& cov, std::string name, Rgbaf color,
                                          BlendMode blendMode, float opacity, PaintOp op,
                                          int cloneOffX, int cloneOffY,
                                          const Selection* selection) {
    std::vector<PaintCommand::DeltaT<Pixel>> deltas;
    Rect dirty{};
    const float colorA = clamp01(color.a);
    const bool gate = selection != nullptr && selection->active();
    for (const auto& [key, buf] : cov) {
        const TileCoord coord{key.first, key.second};
        const int baseX = coord.col * kTileSize;
        const int baseY = coord.row * kTileSize;
        std::shared_ptr<TileDataT<Pixel>> before = store.sharedTile(coord);
        auto after = std::make_shared<TileDataT<Pixel>>();
        if (before) *after = *before;  // start from prior pixels (else transparent)

        bool changed = false;
        for (std::size_t idx = 0; idx < static_cast<std::size_t>(kTilePixels); ++idx) {
            float a = std::min(buf[idx], opacity);
            if (a <= 0.0f) continue;
            if (gate) {
                // Confine the stroke to the active selection (soft edges for free).
                const int lx = static_cast<int>(idx % static_cast<std::size_t>(kTileSize));
                const int ly = static_cast<int>(idx / static_cast<std::size_t>(kTileSize));
                a *= selection->coverage(baseX + lx, baseY + ly);
                if (a <= 0.0f) continue;
            }
            const Rgbaf dst = toFloat(after->px[idx]);
            Rgbaf out = dst;
            switch (op) {
                case PaintOp::Paint: {
                    const Rgbaf src{color.r, color.g, color.b, a * colorA};
                    out = compositeOver(blendMode, dst, src, 1.0f);
                    break;
                }
                case PaintOp::Erase:
                    out.a = dst.a * (1.0f - a);  // reduce alpha
                    break;
                case PaintOp::Dodge:
                case PaintOp::Burn: {
                    // Tone tools adjust RGB of existing content, weighted by coverage; alpha is
                    // preserved and fully transparent pixels are left alone (their RGB is moot).
                    if (dst.a <= 0.0f) continue;
                    const float k = a * kToneExposure;
                    if (op == PaintOp::Dodge) {  // lighten toward white
                        // max(0, 1-dst) keeps dodge monotonic on F32/HDR pixels: a super-white
                        // value (dst > 1, preserved by the float store) is left unchanged rather
                        // than darkened. For the in-gamut [0,1] case this is identical to (1-dst).
                        out.r = dst.r + k * std::max(0.0f, 1.0f - dst.r);
                        out.g = dst.g + k * std::max(0.0f, 1.0f - dst.g);
                        out.b = dst.b + k * std::max(0.0f, 1.0f - dst.b);
                    } else {  // burn: darken toward black (dst*(1-k) shrinks any value toward 0)
                        out.r = dst.r * (1.0f - k);
                        out.g = dst.g * (1.0f - k);
                        out.b = dst.b * (1.0f - k);
                    }
                    break;
                }
                case PaintOp::Clone: {
                    // Sample the layer's pre-stroke pixels (the store is at its S0 state during the
                    // flush — the controller reverts the preview before each rebuild, so there is
                    // no feedback even where source and dest overlap) at the fixed stroke offset,
                    // and composite over the destination at the brush coverage.
                    const int lx = static_cast<int>(idx % static_cast<std::size_t>(kTileSize));
                    const int ly = static_cast<int>(idx / static_cast<std::size_t>(kTileSize));
                    const Rgbaf srcF =
                        toFloat(store.pixel(baseX + lx - cloneOffX, baseY + ly - cloneOffY));
                    out = compositeOver(blendMode, dst, srcF, a);
                    break;
                }
            }
            const Pixel newPx = fromFloat<Pixel>(out);
            if (!pixelEqual(newPx, after->px[idx])) {
                after->px[idx] = newPx;
                changed = true;
            }
        }
        if (changed) {
            deltas.push_back(
                PaintCommand::DeltaT<Pixel>{coord, std::move(before), std::move(after)});
            dirty = dirty.united(tileBounds(coord));
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}

std::unique_ptr<PaintCommand> buildStroke(Document& doc, LayerId layerId, const BrushSettings& in,
                                          Rgbaf color, std::span<const StrokePoint> points,
                                          PaintOp op, std::string name, int cloneOffX,
                                          int cloneOffY, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (points.empty()) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);

    float diameter = std::isfinite(in.diameter) ? in.diameter : 20.0f;
    diameter = std::clamp(diameter, 0.1f, kMaxBrushDiameter);
    const float hardness = clamp01(in.hardness);
    const float opacity = clamp01(in.opacity);
    const float flow = clamp01(in.flow);
    const float spacing = std::max(0.01f, in.spacing);
    const float step = std::max(1.0f, spacing * diameter);

    CoverageMap cov;
    int64_t dabCount = 0;
    const auto dab = [&](Vec2 p, float pressure) {
        if (dabCount >= kMaxDabsPerStroke) return;
        ++dabCount;
        float diam = diameter;
        if (in.pressureControlsSize) diam = diameter * clamp01(pressure);
        const float radius = std::max(0.5f, diam * 0.5f);
        stampDab(cov, p, radius, hardness, flow);
    };

    // Optional stabilization: exponential smoothing of the path (opt-in via
    // BrushSettings::stabilize). Only allocate when enabled, and clamp the lag below
    // 1.0 so a value of exactly 1 can't collapse the whole stroke onto the first
    // point (alpha == 1 would make every smoothed sample equal points[0]).
    std::vector<StrokePoint> stabPoints;
    std::span<const StrokePoint> usePoints = points;
    if (in.stabilize > 0.0f && !points.empty()) {
        const float alpha = std::min(in.stabilize, 0.95f);
        stabPoints.assign(points.begin(), points.end());
        Vec2 last = points[0].pos;
        for (std::size_t i = 0; i < points.size(); ++i) {
            last.x = last.x * alpha + points[i].pos.x * (1.0f - alpha);
            last.y = last.y * alpha + points[i].pos.y * (1.0f - alpha);
            stabPoints[i].pos = last;
        }
        usePoints = stabPoints;
    }

    // First dab at the start, then one every `step` of arc length.
    if (!usePoints.empty()) dab(usePoints[0].pos, usePoints[0].pressure);
    float distSinceLast = 0.0f;
    for (std::size_t i = 1; i < usePoints.size(); ++i) {
        const StrokePoint& a = usePoints[i - 1];
        const StrokePoint& b = usePoints[i];
        const float dx = b.pos.x - a.pos.x;
        const float dy = b.pos.y - a.pos.y;
        const float segLen = std::sqrt(dx * dx + dy * dy);
        if (!(segLen > 1e-6f)) continue;  // also skips NaN (non-finite input points)
        float consumed = 0.0f;
        while (distSinceLast + (segLen - consumed) >= step && dabCount < kMaxDabsPerStroke) {
            const float advance = step - distSinceLast;
            consumed += advance;
            const float t = consumed / segLen;
            dab(Vec2{a.pos.x + dx * t, a.pos.y + dy * t},
                a.pressure + (b.pressure - a.pressure) * t);
            distSinceLast = 0.0f;
        }
        distSinceLast += (segLen - consumed);
    }

    // Flush the accumulated coverage into native-depth tile deltas (one composite,
    // capped at opacity), editing the layer's store at its own bit depth.
    switch (pl->depth()) {
        case BitDepth::U16:
            return flushStroke<Rgba16>(layerId, pl->tiles16(), cov, std::move(name), color,
                                       in.blendMode, opacity, op, cloneOffX, cloneOffY, selection);
        case BitDepth::F32:
            return flushStroke<Rgbaf>(layerId, pl->tilesF(), cov, std::move(name), color,
                                      in.blendMode, opacity, op, cloneOffX, cloneOffY, selection);
        case BitDepth::U8:
        default:
            return flushStroke<Rgba8>(layerId, pl->tiles(), cov, std::move(name), color,
                                      in.blendMode, opacity, op, cloneOffX, cloneOffY, selection);
    }
}

}  // namespace

PaintCommand::PaintCommand(LayerId layer, Rect dirtyRect, std::vector<Delta> deltas,
                           std::string name)
    : layer_(layer),
      dirty_(dirtyRect),
      depth_(BitDepth::U8),
      deltas8_(std::move(deltas)),
      name_(std::move(name)) {}

PaintCommand::PaintCommand(LayerId layer, Rect dirtyRect, std::vector<Delta16> deltas,
                           std::string name)
    : layer_(layer),
      dirty_(dirtyRect),
      depth_(BitDepth::U16),
      deltas16_(std::move(deltas)),
      name_(std::move(name)) {}

PaintCommand::PaintCommand(LayerId layer, Rect dirtyRect, std::vector<DeltaF> deltas,
                           std::string name)
    : layer_(layer),
      dirty_(dirtyRect),
      depth_(BitDepth::F32),
      deltasF_(std::move(deltas)),
      name_(std::move(name)) {}

namespace {
// Swap each delta's snapshot into the store: `after` on execute, `before` on undo.
template <class Store, class DeltaVec>
void applyDeltas(Store& store, const DeltaVec& deltas, bool forward) {
    for (const auto& d : deltas) store.setTile(d.coord, forward ? d.after : d.before);
}
}  // namespace

DocumentChange PaintCommand::apply(Document& doc, bool forward) {
    Layer* layer = doc.findLayer(layer_);
    if (layer != nullptr && layer->kind() == LayerKind::Pixel) {
        auto* pl = static_cast<PixelLayer*>(layer);
        switch (depth_) {
            case BitDepth::U16:
                applyDeltas(pl->tiles16(), deltas16_, forward);
                break;
            case BitDepth::F32:
                applyDeltas(pl->tilesF(), deltasF_, forward);
                break;
            case BitDepth::U8:
            default:
                applyDeltas(pl->tiles(), deltas8_, forward);
                break;
        }
    }
    return DocumentChange{DocumentChange::Kind::Pixels, dirty_, layer_};
}

DocumentChange PaintCommand::execute(Document& doc) {
    return apply(doc, /*forward=*/true);
}

DocumentChange PaintCommand::undo(Document& doc) {
    return apply(doc, /*forward=*/false);
}

std::size_t PaintCommand::touchedTileCount() const noexcept {
    switch (depth_) {
        case BitDepth::U16:
            return deltas16_.size();
        case BitDepth::F32:
            return deltasF_.size();
        case BitDepth::U8:
        default:
            return deltas8_.size();
    }
}

std::unique_ptr<PaintCommand> paintStroke(Document& doc, LayerId layerId,
                                          const BrushSettings& settings, Rgbaf color,
                                          std::span<const StrokePoint> points,
                                          const Selection* selection) {
    return buildStroke(doc, layerId, settings, color, points, PaintOp::Paint, "Brush", 0, 0,
                       selection);
}

std::unique_ptr<PaintCommand> eraseStroke(Document& doc, LayerId layerId,
                                          const BrushSettings& settings,
                                          std::span<const StrokePoint> points,
                                          const Selection* selection) {
    return buildStroke(doc, layerId, settings, Rgbaf{}, points, PaintOp::Erase, "Eraser", 0, 0,
                       selection);
}

std::unique_ptr<PaintCommand> dodgeStroke(Document& doc, LayerId layerId,
                                          const BrushSettings& settings,
                                          std::span<const StrokePoint> points,
                                          const Selection* selection) {
    return buildStroke(doc, layerId, settings, Rgbaf{}, points, PaintOp::Dodge, "Dodge", 0, 0,
                       selection);
}

std::unique_ptr<PaintCommand> burnStroke(Document& doc, LayerId layerId,
                                         const BrushSettings& settings,
                                         std::span<const StrokePoint> points,
                                         const Selection* selection) {
    return buildStroke(doc, layerId, settings, Rgbaf{}, points, PaintOp::Burn, "Burn", 0, 0,
                       selection);
}

std::unique_ptr<PaintCommand> cloneStroke(Document& doc, LayerId layerId,
                                          const BrushSettings& settings,
                                          std::span<const StrokePoint> points, int offsetX,
                                          int offsetY, const Selection* selection) {
    return buildStroke(doc, layerId, settings, Rgbaf{}, points, PaintOp::Clone, "Clone Stamp",
                       offsetX, offsetY, selection);
}

}  // namespace pe
