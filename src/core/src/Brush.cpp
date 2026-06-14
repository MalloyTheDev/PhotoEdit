#include "pe/core/Brush.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"

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

enum class PaintOp { Paint, Erase };

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

std::unique_ptr<PaintCommand> buildStroke(Document& doc, LayerId layerId, const BrushSettings& in,
                                          Rgbaf color, std::span<const StrokePoint> points,
                                          PaintOp op, std::string name) {
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

    // First dab at the start, then one every `step` of arc length.
    dab(points[0].pos, points[0].pressure);
    float distSinceLast = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const StrokePoint& a = points[i - 1];
        const StrokePoint& b = points[i];
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

    // Flush the stroke buffer into tile deltas (one composite, capped at opacity).
    std::vector<PaintCommand::Delta> deltas;
    Rect dirty{};
    const float colorA = clamp01(color.a);
    for (const auto& [key, buf] : cov) {
        const TileCoord coord{key.first, key.second};
        std::shared_ptr<TileData> before = pl->tiles().sharedTile(coord);
        auto after = std::make_shared<TileData>();
        if (before) *after = *before;  // start from prior pixels (else transparent)

        bool changed = false;
        for (std::size_t idx = 0; idx < static_cast<std::size_t>(kTilePixels); ++idx) {
            const float a = std::min(buf[idx], opacity);
            if (a <= 0.0f) continue;
            const Rgbaf dst = toFloat(after->px[idx]);
            Rgbaf out;
            if (op == PaintOp::Paint) {
                const Rgbaf src{color.r, color.g, color.b, a * colorA};
                out = compositeOver(in.blendMode, dst, src, 1.0f);
            } else {
                out = dst;
                out.a = dst.a * (1.0f - a);  // erase: reduce alpha
            }
            const Rgba8 newPx = toRgba8(out);
            if (!(newPx == after->px[idx])) {
                after->px[idx] = newPx;
                changed = true;
            }
        }
        if (changed) {
            deltas.push_back(PaintCommand::Delta{coord, std::move(before), std::move(after)});
            dirty = dirty.united(tileBounds(coord));
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}

}  // namespace

PaintCommand::PaintCommand(LayerId layer, Rect dirtyRect, std::vector<Delta> deltas,
                           std::string name)
    : layer_(layer), dirty_(dirtyRect), deltas_(std::move(deltas)), name_(std::move(name)) {}

DocumentChange PaintCommand::execute(Document& doc) {
    Layer* layer = doc.findLayer(layer_);
    if (layer != nullptr && layer->kind() == LayerKind::Pixel) {
        auto* pl = static_cast<PixelLayer*>(layer);
        for (const Delta& d : deltas_) pl->tiles().setTile(d.coord, d.after);
    }
    return DocumentChange{DocumentChange::Kind::Pixels, dirty_, layer_};
}

DocumentChange PaintCommand::undo(Document& doc) {
    Layer* layer = doc.findLayer(layer_);
    if (layer != nullptr && layer->kind() == LayerKind::Pixel) {
        auto* pl = static_cast<PixelLayer*>(layer);
        for (const Delta& d : deltas_) pl->tiles().setTile(d.coord, d.before);
    }
    return DocumentChange{DocumentChange::Kind::Pixels, dirty_, layer_};
}

std::unique_ptr<PaintCommand> paintStroke(Document& doc, LayerId layerId,
                                          const BrushSettings& settings, Rgbaf color,
                                          std::span<const StrokePoint> points) {
    return buildStroke(doc, layerId, settings, color, points, PaintOp::Paint, "Brush");
}

std::unique_ptr<PaintCommand> eraseStroke(Document& doc, LayerId layerId,
                                          const BrushSettings& settings,
                                          std::span<const StrokePoint> points) {
    return buildStroke(doc, layerId, settings, Rgbaf{}, points, PaintOp::Erase, "Eraser");
}

}  // namespace pe
