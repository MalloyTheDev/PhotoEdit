#include "pe/core/Brush.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"  // gaussianBlur kernel + bakePixelEditRegion (Blur brush)
#include "pe/core/Mask.hpp"    // Mask / MaskBuffer (mask brush)
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

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

// Stamp one dab's coverage into `cov`. If `touched` is non-null, every tile key written to is
// recorded there (the incremental LiveStroke uses it to re-composite only the tiles a new dab hit).
void stampDab(CoverageMap& cov, Vec2 center, float radius, float hardness, float flow,
              std::set<CoverageKey>* touched = nullptr) {
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
            if (touched != nullptr) touched->insert(key);
            std::vector<float>& buf = it->second;

            const int lx = x - col * kTileSize;
            const int ly = y - row * kTileSize;
            float& b = buf[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)];
            b = b + c * (1.0f - b);  // Porter-Duff 'over' on stroke coverage (caps at 1)
        }
    }
}

// One pixel of a brush op: given the destination pixel `dst` and the effective coverage `a`
// (already capped at opacity and gated by the selection), write the new pixel into `out`. Returns
// false to leave the pixel unchanged (Dodge/Burn skip fully-transparent pixels). `sampleS0(x, y)`
// yields the PRE-STROKE pixel at a document coordinate — Clone's source; the batched flush reads
// the store (which is at S0 during the flush), the incremental LiveStroke reads its per-tile
// snapshot. Shared by both paths so their output is byte-identical. (px, py) is the destination
// document coordinate.
template <class SampleS0>
[[nodiscard]] bool brushOpPixel(PaintOp op, const Rgbaf& dst, float a, const Rgbaf& color,
                                float colorA, BlendMode blendMode, int px, int py, int cloneOffX,
                                int cloneOffY, SampleS0&& sampleS0, Rgbaf& out) {
    out = dst;
    switch (op) {
        case PaintOp::Paint: {
            const Rgbaf src{color.r, color.g, color.b, a * colorA};
            out = compositeOver(blendMode, dst, src, 1.0f);
            return true;
        }
        case PaintOp::Erase:
            out.a = dst.a * (1.0f - a);  // reduce alpha
            return true;
        case PaintOp::Dodge:
        case PaintOp::Burn: {
            // Tone tools adjust RGB of existing content, weighted by coverage; alpha is preserved
            // and fully transparent pixels are left alone (their RGB is moot).
            if (dst.a <= 0.0f) return false;
            const float k = a * kToneExposure;
            if (op == PaintOp::Dodge) {  // lighten toward white
                // max(0, 1-dst) keeps dodge monotonic on F32/HDR pixels (super-white left alone).
                out.r = dst.r + k * std::max(0.0f, 1.0f - dst.r);
                out.g = dst.g + k * std::max(0.0f, 1.0f - dst.g);
                out.b = dst.b + k * std::max(0.0f, 1.0f - dst.b);
            } else {  // burn: darken toward black
                out.r = dst.r * (1.0f - k);
                out.g = dst.g * (1.0f - k);
                out.b = dst.b * (1.0f - k);
            }
            return true;
        }
        case PaintOp::Clone: {
            // Sample the PRE-STROKE source pixel at the fixed stroke offset (no feedback even where
            // source and dest overlap) and composite over the destination at the brush coverage.
            // Gate the source coord range (cloneStroke is callable with an arbitrary offset) so the
            // int64->int cast can't overflow; outside it there is no content, so read transparent.
            constexpr int64_t kCloneSampleBound = 1 << 26;  // ~67M, well within int
            const int64_t sx = static_cast<int64_t>(px) - static_cast<int64_t>(cloneOffX);
            const int64_t sy = static_cast<int64_t>(py) - static_cast<int64_t>(cloneOffY);
            Rgbaf srcF{};
            if (sx > -kCloneSampleBound && sx < kCloneSampleBound && sy > -kCloneSampleBound &&
                sy < kCloneSampleBound) {
                srcF = sampleS0(static_cast<int>(sx), static_cast<int>(sy));
            }
            if (blendMode == BlendMode::Normal) {
                // Straight-alpha source-over WITHOUT clamping RGB, so cloning preserves HDR/super-
                // white on an F32 layer. Identical to compositeOver(Normal) for in-gamut values.
                const float sa = clamp01(srcF.a) * a;
                const float ba = clamp01(dst.a);
                const float outA = sa + ba * (1.0f - sa);
                if (outA > 0.0f) {
                    const float inv = 1.0f / outA;
                    out.r = (srcF.r * sa + dst.r * ba * (1.0f - sa)) * inv;
                    out.g = (srcF.g * sa + dst.g * ba * (1.0f - sa)) * inv;
                    out.b = (srcF.b * sa + dst.b * ba * (1.0f - sa)) * inv;
                    out.a = outA;
                } else {
                    out = Rgbaf{};
                }
            } else {
                out = compositeOver(blendMode, dst, srcF, a);
            }
            return true;
        }
    }
    return true;
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
        const auto sampleS0 = [&store](int x, int y) { return toFloat(store.pixel(x, y)); };
        for (std::size_t idx = 0; idx < static_cast<std::size_t>(kTilePixels); ++idx) {
            float a = std::min(buf[idx], opacity);
            if (a <= 0.0f) continue;
            const int lx = static_cast<int>(idx % static_cast<std::size_t>(kTileSize));
            const int ly = static_cast<int>(idx / static_cast<std::size_t>(kTileSize));
            const int px = baseX + lx;
            const int py = baseY + ly;
            if (gate) {
                // Confine the stroke to the active selection (soft edges for free).
                a *= selection->coverage(px, py);
                if (a <= 0.0f) continue;
            }
            const Rgbaf dst = toFloat(after->px[idx]);
            Rgbaf out;
            if (!brushOpPixel(op, dst, a, color, colorA, blendMode, px, py, cloneOffX, cloneOffY,
                              sampleS0, out)) {
                continue;
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

// Rasterize a brush stroke (path of points + BrushSettings) into a per-tile coverage map. Shared by
// every brush op (Paint/Erase/Dodge/Burn/Clone via flushStroke, and Blur via the region bake): the
// coverage in [0,1] is the per-pixel brush footprint along the path, before any op-specific blend.
CoverageMap buildCoverage(const BrushSettings& in, std::span<const StrokePoint> points) {
    float diameter = std::isfinite(in.diameter) ? in.diameter : 20.0f;
    diameter = std::clamp(diameter, 0.1f, kMaxBrushDiameter);
    const float hardness = clamp01(in.hardness);
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
    return cov;
}

std::unique_ptr<PaintCommand> buildStroke(Document& doc, LayerId layerId, const BrushSettings& in,
                                          Rgbaf color, std::span<const StrokePoint> points,
                                          PaintOp op, std::string name, int cloneOffX,
                                          int cloneOffY, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (points.empty()) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);

    const float opacity = clamp01(in.opacity);
    const CoverageMap cov = buildCoverage(in, points);

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

// ---- Blur brush ----
// The Blur brush cannot use the per-pixel flushStroke path because blurring needs a NEIGHBORHOOD.
// Instead it rasterizes the stroke coverage (buildCoverage), takes the pixel-tight bounding box of
// nonzero coverage, and runs ONE region bake (bakePixelEditRegion, the same reversible tile-delta
// machinery behind destructive filters): the region is gaussian-blurred once and each pixel is
// blended toward its blurred value by its accumulated brush coverage (capped at the stroke
// opacity). The bake applies any active selection on top (lerp toward original by 1-coverage), so
// the effective strength is brushCoverage * selectionCoverage — the selection is honored for free.

// Fixed blur kernel for the brush (a small, soft local blur). Matches the task's sigma ~= 2.
constexpr float kBlurSigma = 2.0f;

// Fixed unsharp-mask parameters for the Sharpen brush (mirror of the Blur brush): a small radius,
// a moderate amount, and no threshold so every detail is enhanced. A v1 strength tuned to give a
// visible local contrast boost while the brush coverage/opacity still modulate it per pixel.
constexpr float kSharpenRadius = 1.5f;
constexpr float kSharpenAmount = 1.0f;
constexpr float kSharpenThreshold = 0.0f;

// Pixel-tight bounding box (document space) of all nonzero coverage; empty if the stroke is empty.
Rect coverageBounds(const CoverageMap& cov) {
    Rect bb{};
    for (const auto& [key, buf] : cov) {
        const int baseX = key.first * kTileSize;
        const int baseY = key.second * kTileSize;
        for (int ly = 0; ly < kTileSize; ++ly) {
            for (int lx = 0; lx < kTileSize; ++lx) {
                if (buf[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] >
                    0.0f) {
                    const Rect px{baseX + lx, baseY + ly, 1, 1};
                    bb = bb.united(px);
                }
            }
        }
    }
    return bb;
}

// Look up the accumulated coverage at document-space (x, y); 0 outside touched tiles.
float coverageAt(const CoverageMap& cov, int x, int y) {
    const int col = floorDiv(x, kTileSize);
    const int row = floorDiv(y, kTileSize);
    const auto it = cov.find(CoverageKey{col, row});
    if (it == cov.end()) return 0.0f;
    const int lx = x - col * kTileSize;
    const int ly = y - row * kTileSize;
    return it->second[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)];
}

// Incremental live stroke for the per-pixel ops (see LiveStroke in Brush.hpp). Holds the
// accumulated coverage and a pre-stroke snapshot of every touched tile; extend() stamps only the
// new dabs and re-composites only the tiles a new dab hit, each from its snapshot with the full
// coverage — so the result is byte-identical to the batched flushStroke, just linear over the
// stroke. Stabilization is NOT supported here (callers use the batched path when
// BrushSettings::stabilize > 0).
template <class Pixel>
class LiveStrokeImpl final : public LiveStroke {
public:
    LiveStrokeImpl(LayerId layer, TileStoreT<Pixel>& store, const BrushSettings& brush, Rgbaf color,
                   PaintOp op, std::string name, int cloneOffX, int cloneOffY,
                   const Selection* selection)
        : layer_(layer),
          store_(store),
          color_(color),
          blendMode_(brush.blendMode),
          op_(op),
          name_(std::move(name)),
          cloneOffX_(cloneOffX),
          cloneOffY_(cloneOffY),
          selection_(selection),
          hardness_(clamp01(brush.hardness)),
          flow_(clamp01(brush.flow)),
          opacity_(clamp01(brush.opacity)),
          pressureSize_(brush.pressureControlsSize) {
        const float d = std::isfinite(brush.diameter) ? brush.diameter : 20.0f;
        diameter_ = std::clamp(d, 0.1f, kMaxBrushDiameter);
        step_ = std::max(1.0f, std::max(0.01f, brush.spacing) * diameter_);
    }

    Rect extend(std::span<const StrokePoint> points) override {
        std::set<CoverageKey> touched;
        stampNew(points, touched);
        return flushTiles(touched);
    }

    std::unique_ptr<PaintCommand> finish() override {
        std::vector<typename PaintCommand::DeltaT<Pixel>> deltas;
        Rect dirty{};
        for (const CoverageKey& key : changed_) {
            const TileCoord coord{key.first, key.second};
            deltas.push_back(
                typename PaintCommand::DeltaT<Pixel>{coord, s0_[key], store_.sharedTile(coord)});
            dirty = dirty.united(tileBounds(coord));
        }
        if (deltas.empty()) return nullptr;
        return std::make_unique<PaintCommand>(layer_, dirty, std::move(deltas), name_);
    }

    void cancel() override {
        for (auto& [key, snap] : s0_) store_.setTile(TileCoord{key.first, key.second}, snap);
        s0_.clear();
        cov_.clear();
        changed_.clear();
    }

private:
    // Capture the tile's pre-stroke content the first time the stroke touches it (the store still
    // holds S0 then, since flushTiles only writes tiles AFTER snapshotting). The shared_ptr is COW:
    // a later setTile() replaces the pointer, so the snapshot keeps pointing at the original
    // pixels.
    void ensureS0(const CoverageKey& key) {
        if (s0_.find(key) == s0_.end()) {
            s0_.emplace(key, store_.sharedTile(TileCoord{key.first, key.second}));
        }
    }

    // Place the dabs for the path samples not yet consumed, mirroring buildCoverage's stepping
    // (carrying prevPos_/distSinceLast_ across calls) so the same dab sequence — hence the same
    // accumulated coverage — is produced as the batched path.
    void stampNew(std::span<const StrokePoint> points, std::set<CoverageKey>& touched) {
        const auto dab = [&](Vec2 p, float pressure) {
            if (dabCount_ >= kMaxDabsPerStroke) return;
            ++dabCount_;
            const float diam = pressureSize_ ? diameter_ * clamp01(pressure) : diameter_;
            stampDab(cov_, p, std::max(0.5f, diam * 0.5f), hardness_, flow_, &touched);
        };
        std::size_t i = consumed_;
        if (!started_ && i < points.size()) {
            dab(points[0].pos, points[0].pressure);
            prevPos_ = points[0].pos;
            prevPressure_ = points[0].pressure;
            started_ = true;
            i = 1;
        }
        for (; i < points.size(); ++i) {
            const Vec2 bpos = points[i].pos;
            const float bpr = points[i].pressure;
            const float dx = bpos.x - prevPos_.x;
            const float dy = bpos.y - prevPos_.y;
            const float segLen = std::sqrt(dx * dx + dy * dy);
            if (segLen > 1e-6f) {
                float consumed = 0.0f;
                while (distSinceLast_ + (segLen - consumed) >= step_ &&
                       dabCount_ < kMaxDabsPerStroke) {
                    const float advance = step_ - distSinceLast_;
                    consumed += advance;
                    const float t = consumed / segLen;
                    dab(Vec2{prevPos_.x + dx * t, prevPos_.y + dy * t},
                        prevPressure_ + (bpr - prevPressure_) * t);
                    distSinceLast_ = 0.0f;
                }
                distSinceLast_ += (segLen - consumed);
            }
            prevPos_ = bpos;
            prevPressure_ = bpr;
        }
        consumed_ = points.size();
    }

    // Re-composite each tile whose coverage changed this extend, from its pre-stroke snapshot with
    // the FULL accumulated coverage (same as flushStroke does per tile). Returns the dirtied
    // region.
    Rect flushTiles(const std::set<CoverageKey>& touched) {
        Rect dirty{};
        const float colorA = clamp01(color_.a);
        const bool gate = selection_ != nullptr && selection_->active();
        // Clone reads its source from S0: the snapshot if the tile was touched, else the live store
        // (an untouched tile still holds its pre-stroke pixels).
        const auto sampleS0 = [this](int x, int y) -> Rgbaf {
            const int col = floorDiv(x, kTileSize);
            const int row = floorDiv(y, kTileSize);
            const auto it = s0_.find(CoverageKey{col, row});
            if (it != s0_.end()) {
                if (!it->second) return Rgbaf{};
                const int lx = x - col * kTileSize;
                const int ly = y - row * kTileSize;
                return toFloat(it->second->px[static_cast<std::size_t>(ly) * kTileSize +
                                              static_cast<std::size_t>(lx)]);
            }
            return toFloat(store_.pixel(x, y));
        };
        for (const CoverageKey& key : touched) {
            ensureS0(key);  // snapshot S0 before we overwrite the tile
            const TileCoord coord{key.first, key.second};
            const int baseX = coord.col * kTileSize;
            const int baseY = coord.row * kTileSize;
            const std::vector<float>& buf = cov_.at(key);
            auto out = std::make_shared<TileDataT<Pixel>>();
            if (s0_[key])
                *out = *s0_[key];  // start from the pre-stroke snapshot (else transparent)
            bool changed = false;
            for (std::size_t idx = 0; idx < static_cast<std::size_t>(kTilePixels); ++idx) {
                float a = std::min(buf[idx], opacity_);
                if (a <= 0.0f) continue;
                const int lx = static_cast<int>(idx % static_cast<std::size_t>(kTileSize));
                const int ly = static_cast<int>(idx / static_cast<std::size_t>(kTileSize));
                const int px = baseX + lx;
                const int py = baseY + ly;
                if (gate) {
                    a *= selection_->coverage(px, py);
                    if (a <= 0.0f) continue;
                }
                const Rgbaf dst = toFloat(out->px[idx]);
                Rgbaf o;
                if (!brushOpPixel(op_, dst, a, color_, colorA, blendMode_, px, py, cloneOffX_,
                                  cloneOffY_, sampleS0, o)) {
                    continue;
                }
                const Pixel np = fromFloat<Pixel>(o);
                if (!pixelEqual(np, out->px[idx])) {
                    out->px[idx] = np;
                    changed = true;
                }
            }
            if (changed) {
                // Mirror the batched flushStroke: write the store ONLY when a pixel actually moved.
                // Writing an unchanged tile would materialize a present (often all-transparent)
                // tile where the batched path leaves it ABSENT — e.g. an Erase/Dodge/Burn/Clone dab
                // whose footprint clips a tile that is absent at S0 and stays transparent. That
                // spurious tile is not in changed_, so finish() never captures it and undo cannot
                // remove it, and it needlessly breaks the snapshot's COW sharing. Gating on
                // `changed` is safe: coverage is monotonic non-decreasing and every flush
                // recomposites the full tile from S0, so a tile only ever transitions
                // absent->changed, never changed->unchanged; a tile written in an earlier extend
                // always recomputes changed==true on re-flush.
                store_.setTile(coord,
                               out);  // apply the freshly composited tile to the layer (live)
                changed_.insert(key);
                dirty = dirty.united(tileBounds(coord));
            }
        }
        return dirty;
    }

    LayerId layer_;
    TileStoreT<Pixel>& store_;
    Rgbaf color_;
    BlendMode blendMode_;
    PaintOp op_;
    std::string name_;
    int cloneOffX_;
    int cloneOffY_;
    const Selection* selection_;
    float hardness_;
    float flow_;
    float opacity_;
    bool pressureSize_;
    float diameter_ = 20.0f;
    float step_ = 1.0f;

    CoverageMap cov_;                                              // accumulated coverage
    std::map<CoverageKey, std::shared_ptr<TileDataT<Pixel>>> s0_;  // pre-stroke snapshot per tile
    std::set<CoverageKey> changed_;  // tiles whose pixels actually changed (the committed deltas)
    std::size_t consumed_ = 0;       // input points already stamped
    bool started_ = false;
    Vec2 prevPos_{};
    float prevPressure_ = 1.0f;
    float distSinceLast_ = 0.0f;
    int64_t dabCount_ = 0;
};

}  // namespace

std::unique_ptr<PaintCommand> blurStroke(Document& doc, LayerId layerId, const BrushSettings& in,
                                         std::span<const StrokePoint> points,
                                         const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (points.empty()) return nullptr;

    const float opacity = clamp01(in.opacity);
    const CoverageMap cov = buildCoverage(in, points);
    const Rect bb = coverageBounds(cov);
    if (bb.isEmpty()) return nullptr;  // empty / no-coverage stroke deposits nothing

    // Blend each pixel toward its gaussian-blurred value by the brush coverage (capped at the
    // stroke opacity). The store is at its pre-stroke (S0) state during the bake, so the blur is
    // computed from the original pixels — no feedback across rebuilds of a live preview.
    const int left = bb.left();
    const int top = bb.top();
    return bakePixelEditRegion(
        doc, layerId, "Blur Brush", bb,
        [&cov, opacity, left, top](std::span<Rgbaf> img, int w, int h) {
            std::vector<Rgbaf> blurred(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            gaussianBlur(img, blurred, w, h, kBlurSigma);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float c = std::min(coverageAt(cov, left + x, top + y), opacity);
                    if (c <= 0.0f) continue;
                    const std::size_t i =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(x);
                    const Rgbaf o = img[i];
                    const Rgbaf b = blurred[i];
                    img[i] = Rgbaf{o.r + (b.r - o.r) * c, o.g + (b.g - o.g) * c,
                                   o.b + (b.b - o.b) * c, o.a + (b.a - o.a) * c};
                }
            }
        },
        selection);
}

std::unique_ptr<PaintCommand> sharpenStroke(Document& doc, LayerId layerId, const BrushSettings& in,
                                            std::span<const StrokePoint> points,
                                            const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (points.empty()) return nullptr;

    const float opacity = clamp01(in.opacity);
    const CoverageMap cov = buildCoverage(in, points);
    const Rect bb = coverageBounds(cov);
    if (bb.isEmpty()) return nullptr;  // empty / no-coverage stroke deposits nothing

    // Mirror of blurStroke: blend each pixel toward its unsharp-masked value by the brush coverage
    // (capped at the stroke opacity). The store is at its pre-stroke (S0) state during the bake, so
    // the sharpened result is computed from the original pixels — no feedback across rebuilds of a
    // live preview.
    const int left = bb.left();
    const int top = bb.top();
    return bakePixelEditRegion(
        doc, layerId, "Sharpen Brush", bb,
        [&cov, opacity, left, top](std::span<Rgbaf> img, int w, int h) {
            std::vector<Rgbaf> sharpened(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            unsharpMask(img, sharpened, w, h, kSharpenRadius, kSharpenAmount, kSharpenThreshold);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float c = std::min(coverageAt(cov, left + x, top + y), opacity);
                    if (c <= 0.0f) continue;
                    const std::size_t i =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(x);
                    const Rgbaf o = img[i];
                    const Rgbaf s = sharpened[i];
                    img[i] = Rgbaf{o.r + (s.r - o.r) * c, o.g + (s.g - o.g) * c,
                                   o.b + (s.b - o.b) * c, o.a + (s.a - o.a) * c};
                }
            }
        },
        selection);
}

// ---- Mask brush ----
// Paints into the active layer's MASK buffer (single-channel grayscale) rather than its pixels:
// each touched mask byte is blended from its current value toward `targetGray` by the brush
// coverage (capped at the stroke opacity, and gated by any active selection). The before/after
// bytes over the stroke's bounding box are snapshotted so the command is byte-exact reversible.
std::unique_ptr<MaskPaintCommand> maskPaintStroke(Document& doc, LayerId layerId,
                                                  const BrushSettings& in,
                                                  std::span<const StrokePoint> points,
                                                  float targetGray, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr) return nullptr;
    Mask* mask = layer->mask();
    if (mask == nullptr || points.empty()) return nullptr;  // no mask to paint

    const float opacity = clamp01(in.opacity);
    // targetGray is the user-facing value (0 hides, 1 reveals). The buffer stores raw coverage that
    // Mask::evaluate() flips when the mask is inverted, so on an inverted mask we must write the
    // complement to keep "black hides, white reveals" true on the canvas and in the thumbnail.
    float target = clamp01(targetGray);
    if (mask->inverted()) target = 1.0f - target;
    target *= 255.0f;
    const CoverageMap cov = buildCoverage(in, points);
    const Rect bb = coverageBounds(cov);
    if (bb.isEmpty()) return nullptr;
    // Bound the dense snapshot allocation (the bbox can be large for a long diagonal stroke even
    // though coverage is sparse). Over the cap, refuse rather than allocate unboundedly.
    constexpr std::int64_t kMaxMaskBrushPixels = 16'000'000;
    const std::int64_t area = static_cast<std::int64_t>(bb.width) * bb.height;
    if (area > kMaxMaskBrushPixels) return nullptr;

    const int w = bb.width;
    const int h = bb.height;
    const int left = bb.left();
    const int top = bb.top();
    const bool gate = selection != nullptr && selection->active();
    std::vector<std::uint8_t> before(static_cast<std::size_t>(area));
    std::vector<std::uint8_t> after(static_cast<std::size_t>(area));
    bool changed = false;
    MaskBuffer& buf = mask->buffer();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                  static_cast<std::size_t>(x);
            const std::uint8_t b = buf.value(left + x, top + y);
            before[i] = b;
            float c = std::min(coverageAt(cov, left + x, top + y), opacity);
            if (gate) c *= selection->coverage(left + x, top + y);
            std::uint8_t a = b;
            if (c > 0.0f) {
                const float nv = static_cast<float>(b) + (target - static_cast<float>(b)) * c;
                a = static_cast<std::uint8_t>(std::lround(std::clamp(nv, 0.0f, 255.0f)));
            }
            after[i] = a;
            if (a != b) changed = true;
        }
    }
    if (!changed) return nullptr;  // stroke missed the mask entirely
    return std::make_unique<MaskPaintCommand>(layerId, bb, std::move(before), std::move(after));
}

MaskPaintCommand::MaskPaintCommand(LayerId layer, Rect region, std::vector<std::uint8_t> before,
                                   std::vector<std::uint8_t> after)
    : layer_(layer), region_(region), before_(std::move(before)), after_(std::move(after)) {}

DocumentChange MaskPaintCommand::apply(Document& doc, const std::vector<std::uint8_t>& values) {
    Layer* layer = doc.findLayer(layer_);
    // Defensive: if the mask was removed out of lockstep, this is a no-op (never crash).
    if (layer != nullptr && layer->mask() != nullptr &&
        values.size() == static_cast<std::size_t>(region_.width) * region_.height) {
        MaskBuffer& buf = layer->mask()->buffer();
        for (int y = 0; y < region_.height; ++y) {
            for (int x = 0; x < region_.width; ++x) {
                buf.setValue(region_.left() + x, region_.top() + y,
                             values[static_cast<std::size_t>(y) * region_.width + x]);
            }
        }
        // Writing kOpaque cannot erase a tile setValue created, so undoing a stroke that first
        // allocated tiles would otherwise leave redundant fully-revealing tiles behind (growing
        // contentBounds/serialization, defeating the compositor's empty-mask fast path). Drop them
        // so execute/undo are byte-exact at the tile level.
        buf.compact(region_);
    }
    // MaskPixels (not LayerProps): the renderer recomposites region_, and the Layers panel
    // refreshes only this layer's row (thumbnail + mask thumbnail) instead of rebuilding the whole
    // tree.
    return DocumentChange{DocumentChange::Kind::MaskPixels, region_, layer_};
}

DocumentChange MaskPaintCommand::execute(Document& doc) {
    return apply(doc, after_);
}
DocumentChange MaskPaintCommand::undo(Document& doc) {
    return apply(doc, before_);
}

// ---- Spot Healing brush ----
// See Brush.hpp. The painted pixels are refilled by a bounded Gauss-Seidel solve of Laplace's
// equation with the surrounding (unpainted) pixels as the Dirichlet boundary, run in premultiplied
// alpha. kMaxHealRegionPixels bounds the inflated region so both the working memory and the
// iteration cost stay bounded (a brush too large to heal returns nullptr, like an over-budget
// bake).
constexpr int64_t kMaxHealRegionPixels = 2'000'000;

std::unique_ptr<PaintCommand> healStroke(Document& doc, LayerId layerId, const BrushSettings& in,
                                         std::span<const StrokePoint> points,
                                         const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (points.empty()) return nullptr;

    const float opacity = clamp01(in.opacity);
    const CoverageMap cov = buildCoverage(in, points);
    const Rect spot = coverageBounds(cov);
    if (spot.isEmpty()) return nullptr;  // empty / no-coverage stroke heals nothing

    // Inflate the spot to include a ring of surrounding source pixels (the diffusion boundary),
    // clamped to the canvas. The margin grows with the spot so a bigger hole gets a bigger ring.
    const int maxDim = std::max(spot.width, spot.height);
    const int margin = std::clamp(maxDim / 5 + 3, 3, 48);
    const Rect region =
        Rect{spot.x - margin, spot.y - margin, spot.width + 2 * margin, spot.height + 2 * margin}
            .intersected(doc.canvasBounds());
    if (region.isEmpty()) return nullptr;  // spot entirely off-canvas
    const int64_t area = static_cast<int64_t>(region.width) * static_cast<int64_t>(region.height);
    if (area > kMaxHealRegionPixels) return nullptr;  // brush too large to heal

    const int left = region.left();
    const int top = region.top();
    // Iterations scale with the spot size (boundary info propagates ~1px per sweep) but are capped,
    // so a large spot stays bounded — it under-fills toward the surrounding mean, never garbage.
    const int iterations = std::clamp(maxDim / 2 + 8, 8, 64);

    return bakePixelEditRegion(
        doc, layerId, "Spot Healing Brush", region,
        [&cov, opacity, left, top, iterations](std::span<Rgbaf> img, int w, int h) {
            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            std::vector<float> heal(n, 0.0f);      // brush coverage (capped at opacity) per pixel
            std::vector<std::uint8_t> hole(n, 0);  // 1 = painted pixel to refill (unknown)
            std::vector<Rgbaf> p(n);               // premultiplied working buffer for the solve

            // Premultiply, and accumulate the mean of the surrounding (known) pixels.
            Rgbaf knownSum{};
            int64_t knownCount = 0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t i =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(x);
                    const Rgbaf& o = img[i];
                    p[i] = Rgbaf{o.a * o.r, o.a * o.g, o.a * o.b, o.a};  // premultiply
                    const float c = std::min(coverageAt(cov, left + x, top + y), opacity);
                    heal[i] = c;
                    if (c > 0.0f) {
                        hole[i] = 1;
                    } else {
                        knownSum.r += p[i].r;
                        knownSum.g += p[i].g;
                        knownSum.b += p[i].b;
                        knownSum.a += p[i].a;
                        ++knownCount;
                    }
                }
            }
            if (knownCount == 0) return;  // no surrounding context: leave the pixels untouched
            const float invK = 1.0f / static_cast<float>(knownCount);
            const Rgbaf mean{knownSum.r * invK, knownSum.g * invK, knownSum.b * invK,
                             knownSum.a * invK};
            for (std::size_t i = 0; i < n; ++i) {
                if (hole[i] != 0) p[i] = mean;  // seed the hole with the surrounding mean
            }

            // Gauss-Seidel relaxation: each hole pixel relaxes to the mean of its 4-neighbours
            // (known neighbours hold the fixed boundary value). Alternate scan direction per sweep.
            for (int it = 0; it < iterations; ++it) {
                const bool fwd = (it & 1) == 0;
                for (int yy = 0; yy < h; ++yy) {
                    const int y = fwd ? yy : (h - 1 - yy);
                    for (int xx = 0; xx < w; ++xx) {
                        const int x = fwd ? xx : (w - 1 - xx);
                        const std::size_t i =
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                            static_cast<std::size_t>(x);
                        if (hole[i] == 0) continue;
                        Rgbaf acc{};
                        int cnt = 0;
                        const auto add = [&](std::size_t j) {
                            acc.r += p[j].r;
                            acc.g += p[j].g;
                            acc.b += p[j].b;
                            acc.a += p[j].a;
                            ++cnt;
                        };
                        if (x > 0) add(i - 1);
                        if (x < w - 1) add(i + 1);
                        if (y > 0) add(i - static_cast<std::size_t>(w));
                        if (y < h - 1) add(i + static_cast<std::size_t>(w));
                        if (cnt > 0) {
                            const float inv = 1.0f / static_cast<float>(cnt);
                            p[i] = Rgbaf{acc.r * inv, acc.g * inv, acc.b * inv, acc.a * inv};
                        }
                    }
                }
            }

            // Unpremultiply each filled pixel and blend the original toward it by the brush
            // coverage.
            for (std::size_t i = 0; i < n; ++i) {
                if (hole[i] == 0) continue;
                const Rgbaf& pm = p[i];
                // No [0,1] clamp on the fill: HDR / super-white surroundings must survive on an F32
                // layer, exactly as they do for the Blur/Sharpen/Clone siblings (the region bake is
                // depth-generic). The integer stores still clamp on write via
                // fromFloat<Rgba8/Rgba16>, so U8/U16 results are byte-identical. Guard only against
                // a negative/NaN alpha.
                const float fa = pm.a > 0.0f ? pm.a : 0.0f;
                Rgbaf fill{0.0f, 0.0f, 0.0f, fa};
                if (pm.a > 1e-4f) {
                    fill.r = pm.r / pm.a;
                    fill.g = pm.g / pm.a;
                    fill.b = pm.b / pm.a;
                }
                const Rgbaf o = img[i];
                const float c = heal[i];
                img[i] = Rgbaf{o.r + (fill.r - o.r) * c, o.g + (fill.g - o.g) * c,
                               o.b + (fill.b - o.b) * c, o.a + (fill.a - o.a) * c};
            }
        },
        selection);
}

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

LiveStroke::~LiveStroke() = default;

namespace {
// Build a depth-matched LiveStrokeImpl for the layer (nullptr if not a pixel layer), mirroring how
// buildStroke dispatches flushStroke over the store's bit depth.
std::unique_ptr<LiveStroke> beginLive(Document& doc, LayerId layerId, const BrushSettings& settings,
                                      Rgbaf color, PaintOp op, std::string name, int offX, int offY,
                                      const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);
    switch (pl->depth()) {
        case BitDepth::U16:
            return std::make_unique<LiveStrokeImpl<Rgba16>>(layerId, pl->tiles16(), settings, color,
                                                            op, std::move(name), offX, offY,
                                                            selection);
        case BitDepth::F32:
            return std::make_unique<LiveStrokeImpl<Rgbaf>>(
                layerId, pl->tilesF(), settings, color, op, std::move(name), offX, offY, selection);
        case BitDepth::U8:
        default:
            return std::make_unique<LiveStrokeImpl<Rgba8>>(
                layerId, pl->tiles(), settings, color, op, std::move(name), offX, offY, selection);
    }
}
}  // namespace

std::unique_ptr<LiveStroke> beginPaintStroke(Document& doc, LayerId layerId,
                                             const BrushSettings& settings, Rgbaf color,
                                             const Selection* selection) {
    return beginLive(doc, layerId, settings, color, PaintOp::Paint, "Brush", 0, 0, selection);
}

std::unique_ptr<LiveStroke> beginEraseStroke(Document& doc, LayerId layerId,
                                             const BrushSettings& settings,
                                             const Selection* selection) {
    return beginLive(doc, layerId, settings, Rgbaf{}, PaintOp::Erase, "Eraser", 0, 0, selection);
}

std::unique_ptr<LiveStroke> beginDodgeStroke(Document& doc, LayerId layerId,
                                             const BrushSettings& settings,
                                             const Selection* selection) {
    return beginLive(doc, layerId, settings, Rgbaf{}, PaintOp::Dodge, "Dodge", 0, 0, selection);
}

std::unique_ptr<LiveStroke> beginBurnStroke(Document& doc, LayerId layerId,
                                            const BrushSettings& settings,
                                            const Selection* selection) {
    return beginLive(doc, layerId, settings, Rgbaf{}, PaintOp::Burn, "Burn", 0, 0, selection);
}

std::unique_ptr<LiveStroke> beginCloneStroke(Document& doc, LayerId layerId,
                                             const BrushSettings& settings, int offsetX,
                                             int offsetY, const Selection* selection) {
    return beginLive(doc, layerId, settings, Rgbaf{}, PaintOp::Clone, "Clone Stamp", offsetX,
                     offsetY, selection);
}

}  // namespace pe
