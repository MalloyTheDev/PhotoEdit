#pragma once

#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace pe {

// Software (CPU) display-tile cache + dirty-tile compositing pipeline. It observes
// the document, marks the tiles overlapping each change dirty, and recomposites
// ONLY dirty (or uncached) tiles when a region is rendered — so cost scales with
// the changed/visible area, not the document size (ADR-0003).
//
// This is the headless reference for the canvas renderer. The GPU/RHI display path
// (D3D12, ADR-0002) mirrors it tile-for-tile, and the Qt viewport widget drives it
// with a ViewTransform. See docs/systems/02-canvas-rendering.md.
class CanvasRenderer final : public DocumentObserver {
public:
    explicit CanvasRenderer(Document& doc);
    ~CanvasRenderer() override;

    CanvasRenderer(const CanvasRenderer&) = delete;
    CanvasRenderer& operator=(const CanvasRenderer&) = delete;

    // Mark every tile overlapping a document rect as needing recomposite.
    void invalidate(Rect docRect);
    // Drop the whole cache (structural change whose region is unknown).
    void invalidateAll() noexcept;

    // Composite a document region, recompositing only dirty/uncached tiles and
    // reusing cached ones, and return it as an 8-bit image. Returns an empty
    // buffer for an empty or over-budget region.
    [[nodiscard]] PixelBuffer renderRegion(Rect docRect);

    // DocumentObserver: auto-invalidate the affected region on visual changes.
    void onDocumentChanged(const Document&, const DocumentChange&) override;

    // Diagnostics for tests: total tile recomposites, and resident cache size.
    [[nodiscard]] uint64_t recompositeCount() const noexcept { return recompositeCount_; }
    [[nodiscard]] std::size_t cachedTileCount() const noexcept { return cache_.size(); }

private:
    using Key = std::pair<int, int>;
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    struct CachedTile {
        std::array<Rgba8, kTilePixels> px{};
    };

    const CachedTile& ensureTile(TileCoord c);  // composite if dirty/missing

    Document& doc_;
    std::map<Key, CachedTile> cache_;
    std::set<Key> dirty_;
    uint64_t recompositeCount_ = 0;
};

}  // namespace pe
