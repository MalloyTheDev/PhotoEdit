#include "pe/core/CanvasRenderer.hpp"

#include "pe/core/Compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pe {

namespace {
// A single invalidate() spanning more tiles than this drops the whole cache
// instead of iterating (bounds invalidate cost on document-wide changes).
constexpr int64_t kMaxInvalidateTiles = 16384;
}  // namespace

CanvasRenderer::CanvasRenderer(Document& doc)
    : doc_(doc), scratch_(static_cast<std::size_t>(kTilePixels), Rgbaf{}) {
    doc_.addObserver(this);
}

CanvasRenderer::~CanvasRenderer() {
    doc_.removeObserver(this);
}

void CanvasRenderer::setCacheBudgetTiles(std::size_t n) noexcept {
    budgetTiles_ = n;
    evictToBudget();
}

void CanvasRenderer::invalidate(Rect docRect) {
    if (docRect.isEmpty()) return;
    const TileSpan span = tilesForRect(docRect);
    const int64_t cols = static_cast<int64_t>(span.colEnd) - span.colBegin;
    const int64_t rows = static_cast<int64_t>(span.rowEnd) - span.rowBegin;
    if (cols <= 0 || rows <= 0) return;
    if (cols * rows > kMaxInvalidateTiles) {
        invalidateAll();
        return;
    }
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const Key key = keyOf(TileCoord{col, row});
            // Only track cached tiles; an absent tile recomposites on miss anyway.
            if (index_.find(key) != index_.end()) dirty_.insert(key);
        }
    }
}

void CanvasRenderer::invalidateAll() noexcept {
    lru_.clear();
    index_.clear();
    dirty_.clear();
}

void CanvasRenderer::evictToBudget() noexcept {
    if (budgetTiles_ == 0) return;  // unbounded
    while (lru_.size() > budgetTiles_) {
        const Key key = lru_.back().first;
        index_.erase(key);
        dirty_.erase(key);
        lru_.pop_back();
    }
}

void CanvasRenderer::compositeTileUncached(TileCoord c) {
    std::fill(scratch_.begin(), scratch_.end(), Rgbaf{});
    compositeStack(doc_.topLevelLayers(), c, scratch_, 0);
    ++recompositeCount_;
}

const CanvasRenderer::CachedTile& CanvasRenderer::ensureTile(TileCoord c) {
    const Key key = keyOf(c);
    auto idxIt = index_.find(key);
    const bool cached = idxIt != index_.end();
    const bool needComposite = !cached || (dirty_.find(key) != dirty_.end());

    if (cached && !needComposite) {
        lru_.splice(lru_.begin(), lru_, idxIt->second);  // touch (MRU)
        return idxIt->second->second;
    }

    compositeTileUncached(c);  // fills scratch_, bumps recompositeCount_

    CachedTile tile;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTilePixels); ++i) {
        // Display conversion is identity until color management (M6).
        tile.px[i] = toRgba8(scratch_[i]);
    }
    dirty_.erase(key);

    if (cached) {
        idxIt->second->second = std::move(tile);
        lru_.splice(lru_.begin(), lru_, idxIt->second);
        return idxIt->second->second;
    }
    lru_.emplace_front(key, std::move(tile));
    index_[key] = lru_.begin();
    evictToBudget();  // never evicts the just-added front tile (budget >= 1)
    return lru_.front().second;
}

PixelBuffer CanvasRenderer::renderRegion(Rect docRect) {
    if (docRect.isEmpty()) return PixelBuffer{};
    // Bound both total pixels and per-dimension extent (a thin, enormous rect
    // would pass an area cap yet iterate a huge tile column).
    if (docRect.width > kMaxCanvasDimension || docRect.height > kMaxCanvasDimension) {
        return PixelBuffer{};
    }
    // Also bound the ORIGIN: width/height alone don't stop a far-offset rect whose right()/bottom()
    // (x+width) or tilesForRect (col*kTileSize) would signed-overflow int (UB). Mirrors Filter.cpp.
    if (docRect.x > kMaxCanvasDimension || docRect.x < -kMaxCanvasDimension ||
        docRect.y > kMaxCanvasDimension || docRect.y < -kMaxCanvasDimension) {
        return PixelBuffer{};
    }
    const int64_t area = static_cast<int64_t>(docRect.width) * static_cast<int64_t>(docRect.height);
    if (area > kMaxCompositeImagePixels) return PixelBuffer{};

    PixelBuffer out(docRect.width, docRect.height, Rgba8{});
    const TileSpan span = tilesForRect(docRect);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const CachedTile& tile = ensureTile(coord);  // ref valid until next call
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(docRect);
            if (vis.isEmpty()) continue;
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const int ly = y - tb.top();
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const int lx = x - tb.left();
                    out.set(x - docRect.left(), y - docRect.top(),
                            tile.px[static_cast<std::size_t>(ly) * kTileSize +
                                    static_cast<std::size_t>(lx)]);
                }
            }
        }
    }
    return out;
}

PixelBuffer CanvasRenderer::renderRegionScaled(Rect docRegion, int maxOutputPixels) {
    if (docRegion.isEmpty() || maxOutputPixels <= 0) return PixelBuffer{};
    // Same per-dimension extent guard as renderRegion: a thin, enormous rect would
    // otherwise iterate a huge tile column even at a small output size.
    if (docRegion.width > kMaxCanvasDimension || docRegion.height > kMaxCanvasDimension) {
        return PixelBuffer{};
    }
    // Bound the origin too (x+width / col*kTileSize would otherwise signed-overflow int = UB).
    if (docRegion.x > kMaxCanvasDimension || docRegion.x < -kMaxCanvasDimension ||
        docRegion.y > kMaxCanvasDimension || docRegion.y < -kMaxCanvasDimension) {
        return PixelBuffer{};
    }

    // Clamp the output cap to the composite budget so the output raster (and its
    // accumulators) always fit a single eager allocation within engine limits.
    const int64_t cap = std::min<int64_t>(maxOutputPixels, kMaxCompositeImagePixels);
    const int64_t area =
        static_cast<int64_t>(docRegion.width) * static_cast<int64_t>(docRegion.height);

    // Fast path: the region already fits the output cap (hence the composite budget,
    // since cap <= kMaxCompositeImagePixels). Behave exactly as renderRegion — same
    // tile cache, byte-identical result.
    if (area <= cap) return renderRegion(docRegion);

    // Otherwise downscale by an integer factor so the OUTPUT raster is <= cap. s is
    // the smallest factor with area / s^2 <= cap; bump it until the ceil-rounded
    // output also fits (the +1 from ceil can nudge a tight fit just over the cap).
    auto ceilDiv = [](int a, int d) { return (a + d - 1) / d; };
    int s = static_cast<int>(
        std::ceil(std::sqrt(static_cast<double>(area) / static_cast<double>(cap))));
    if (s < 2) s = 2;  // area > cap guarantees a real downscale
    int ow = ceilDiv(docRegion.width, s);
    int oh = ceilDiv(docRegion.height, s);
    while (static_cast<int64_t>(ow) * oh > cap) {
        ++s;
        ow = ceilDiv(docRegion.width, s);
        oh = ceilDiv(docRegion.height, s);
    }

    // Box-average accumulators in premultiplied float (averaging straight-alpha over
    // transparent pixels would bias color toward black). One bin per output pixel,
    // so the working set is bounded by cap (four floats/bin). The full-res region is
    // NEVER materialized — tiles are composited and accumulated one at a time. Per-bin
    // sample COUNT is derived from geometry (below), avoiding a fifth accumulator.
    // Bound compute: unlike renderRegion (area-capped at 64 MP), the scaled path drops the area cap
    // to allow extreme zoom-out, so cap the SOURCE tile count instead — otherwise a near-max-extent
    // region would composite millions of tiles. Beyond the cap, degrade to empty (show pasteboard).
    constexpr int64_t kMaxScaledSourceTiles = 16384;  // ~1 GP of source; ~16x renderRegion's bound
    const TileSpan span = tilesForRect(docRegion);
    const int64_t srcTiles = static_cast<int64_t>(span.rowEnd - span.rowBegin) *
                             static_cast<int64_t>(span.colEnd - span.colBegin);
    if (srcTiles > kMaxScaledSourceTiles) return PixelBuffer{};

    // Box-average accumulators in DOUBLE: a bin can sum up to s*s unit-magnitude samples, which can
    // exceed float32's 2^24 exact-integer limit at large downscale and silently saturate (dropping
    // the running sum while the divisor keeps growing → the average collapses toward 0). double's
    // 53-bit mantissa sums any reachable count exactly. Working set is still bounded by cap.
    const std::size_t outCount = static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh);
    std::vector<double> sumR(outCount, 0.0);
    std::vector<double> sumG(outCount, 0.0);
    std::vector<double> sumB(outCount, 0.0);
    std::vector<double> sumA(outCount, 0.0);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(docRegion);
            if (vis.isEmpty()) continue;
            compositeTileUncached(coord);  // float result in scratch_, no caching
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const int ly = y - tb.top();
                const int oy = (y - docRegion.top()) / s;
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const int lx = x - tb.left();
                    const int ox = (x - docRegion.left()) / s;
                    const std::size_t bin =
                        static_cast<std::size_t>(oy) * static_cast<std::size_t>(ow) +
                        static_cast<std::size_t>(ox);
                    const Rgbaf& c = scratch_[static_cast<std::size_t>(ly) * kTileSize +
                                              static_cast<std::size_t>(lx)];
                    sumR[bin] += c.r * c.a;  // premultiplied
                    sumG[bin] += c.g * c.a;
                    sumB[bin] += c.b * c.a;
                    sumA[bin] += c.a;
                }
            }
        }
    }

    PixelBuffer out(ow, oh, Rgba8{});
    for (int oy = 0; oy < oh; ++oy) {
        // Source rows this output row covers (clamped to the region's height).
        const int srcH = std::min((oy + 1) * s, docRegion.height) - oy * s;
        for (int ox = 0; ox < ow; ++ox) {
            const int srcW = std::min((ox + 1) * s, docRegion.width) - ox * s;
            const std::size_t bin = static_cast<std::size_t>(oy) * static_cast<std::size_t>(ow) +
                                    static_cast<std::size_t>(ox);
            const double n = static_cast<double>(srcW) * static_cast<double>(srcH);
            Rgbaf px{};
            if (n > 0.0) {
                px.a = static_cast<float>(sumA[bin] / n);
                if (sumA[bin] > 0.0) {  // un-premultiply to straight alpha
                    px.r = static_cast<float>(sumR[bin] / sumA[bin]);
                    px.g = static_cast<float>(sumG[bin] / sumA[bin]);
                    px.b = static_cast<float>(sumB[bin] / sumA[bin]);
                }
            }
            out.set(ox, oy, toRgba8(px));
        }
    }
    return out;
}

void CanvasRenderer::onDocumentChanged(const Document&, const DocumentChange& change) {
    switch (change.kind) {
        case DocumentChange::Kind::Pixels:
        case DocumentChange::Kind::MaskPixels:  // a mask edit changes the composite over its region
        case DocumentChange::Kind::LayerProps:
        case DocumentChange::Kind::LayerStructure:
        case DocumentChange::Kind::Profile:  // appearance changes -> recomposite all
            if (change.dirtyRegion.isEmpty()) {
                invalidateAll();  // unknown extent: be safe
            } else {
                invalidate(change.dirtyRegion);
            }
            break;
        case DocumentChange::Kind::ActiveLayer:
        case DocumentChange::Kind::DirtyState:
        case DocumentChange::Kind::Selection:
            break;  // no pixel change (selection only affects the marching-ants overlay)
    }
}

}  // namespace pe
