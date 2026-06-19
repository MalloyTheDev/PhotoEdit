#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace pe {

// Default display-tile cache budget (~128 MB at 256 KB/tile). Covers a large
// viewport plus panning margin; tune per the performance RAM budget (M2+).
inline constexpr std::size_t kDefaultDisplayCacheTiles = 512;

// Default output-pixel cap for renderRegionScaled (~4 MP). The scaled path never
// allocates the full-res region — only the (downsampled) output plus four float
// accumulators of the same pixel count — so this bounds its peak working set (~80
// MB here). Comfortably below the per-call composite budget
// (kMaxCompositeImagePixels) so the downscaled raster always fits a single eager
// allocation; callers wanting sharper zoom-out previews may pass a larger value.
inline constexpr int kDefaultDisplayMaxOutputPixels = 4'000'000;

// Software (CPU) display-tile cache + dirty-tile compositing pipeline. It observes
// the document, marks the tiles overlapping each change dirty, and recomposites
// ONLY dirty/uncached tiles when a region is rendered — so cost scales with the
// changed/visible area, not the document size (ADR-0003).
//
// The cache is an LRU bounded by a tile budget (docs/systems/02-canvas-rendering.md:
// "visible-tile LRU ... eviction is LRU under the RAM budget"), so panning across a
// huge document cannot grow memory without bound.
//
// LIFETIME: `doc` must outlive this renderer — the destructor unregisters the
// observer. This is the headless reference the GPU/RHI display path mirrors
// (ADR-0002), driven by a ViewTransform from the Qt viewport widget.
class CanvasRenderer final : public DocumentObserver {
public:
    explicit CanvasRenderer(Document& doc);
    ~CanvasRenderer() override;

    CanvasRenderer(const CanvasRenderer&) = delete;
    CanvasRenderer& operator=(const CanvasRenderer&) = delete;

    // Mark cached tiles overlapping a document rect as needing recomposite.
    // Uncached tiles are implicitly dirty (recomposited on cache-miss), so they
    // are not tracked — this bounds the dirty set by the cache size. A region
    // spanning more than a threshold of tiles drops the whole cache instead.
    void invalidate(Rect docRect);
    void invalidateAll() noexcept;

    // Composite a document region, recompositing only dirty/uncached tiles and
    // reusing cached ones. Returns an empty buffer for an empty or over-budget
    // region.
    [[nodiscard]] PixelBuffer renderRegion(Rect docRect);

    // Composite a document region for display at a bounded output resolution.
    //
    // If `docRegion` fits both the per-call composite budget and `maxOutputPixels`,
    // this is exactly `renderRegion(docRegion)` (same fast path, same tile cache,
    // byte-identical result — no regression). Otherwise it composites at an integer
    // downscale factor s = ceil(sqrt(area / maxOutputPixels)) and returns a
    // ceil(w/s) x ceil(h/s) buffer that box-averages the region. This lets an
    // EXTREME zoom-out — where the visible region alone exceeds the compositor's
    // budget — still produce a (downsampled) image instead of blanking.
    //
    // Memory is bounded by `maxOutputPixels`: the full-res region is NEVER
    // materialized. Tiles are composited one at a time and box-accumulated into the
    // output, and the scaled pass does NOT touch the display-tile cache (it would
    // otherwise thrash the LRU on a one-off zoom-out). Returns an empty buffer for an
    // empty region, a non-positive cap, or a per-dimension extent over
    // kMaxCanvasDimension. `maxOutputPixels` is itself clamped to the composite
    // budget so the output raster always fits one allocation.
    [[nodiscard]] PixelBuffer renderRegionScaled(
        Rect docRegion, int maxOutputPixels = kDefaultDisplayMaxOutputPixels);

    void onDocumentChanged(const Document&, const DocumentChange&) override;

    // Diagnostics / tuning.
    [[nodiscard]] uint64_t recompositeCount() const noexcept { return recompositeCount_; }
    [[nodiscard]] std::size_t cachedTileCount() const noexcept { return lru_.size(); }
    // 0 means unbounded (no eviction) — be deliberate; never set from untrusted input.
    void setCacheBudgetTiles(std::size_t n) noexcept;

private:
    using Key = std::pair<int, int>;
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    struct CachedTile {
        std::array<Rgba8, kTilePixels> px{};
    };
    using LruList = std::list<std::pair<Key, CachedTile>>;  // front == most recent

    const CachedTile& ensureTile(TileCoord c);  // composite if dirty/missing
    void evictToBudget() noexcept;

    // Composite one tile straight into `scratch_` WITHOUT caching it (used by the
    // scaled path so a zoom-out pass does not pollute/thrash the display-tile LRU).
    void compositeTileUncached(TileCoord c);

    Document& doc_;
    LruList lru_;
    std::map<Key, LruList::iterator> index_;
    std::set<Key> dirty_;
    std::vector<Rgbaf> scratch_;  // reused per-tile composite buffer
    std::size_t budgetTiles_ = kDefaultDisplayCacheTiles;
    uint64_t recompositeCount_ = 0;
};

}  // namespace pe
