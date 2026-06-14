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

    Document& doc_;
    LruList lru_;
    std::map<Key, LruList::iterator> index_;
    std::set<Key> dirty_;
    std::vector<Rgbaf> scratch_;  // reused per-tile composite buffer
    std::size_t budgetTiles_ = kDefaultDisplayCacheTiles;
    uint64_t recompositeCount_ = 0;
};

}  // namespace pe
