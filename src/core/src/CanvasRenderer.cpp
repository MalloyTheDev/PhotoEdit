#include "pe/core/CanvasRenderer.hpp"

#include "pe/core/Compositor.hpp"
#include "pe/core/Performance.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

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
    // Also respect global RAM budget from Performance layer skeleton
    auto& perf = Performance::instance();
    perf.setRAMBudgetBytes(n * sizeof(Rgba8) * kTilePixels);  // rough
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
    auto& perf = Performance::instance();
    auto& scratch = perf.scratch();
    while (lru_.size() > budgetTiles_ || perf.overBudget(lru_.size() * sizeof(Rgba8) * kTilePixels)) {
        const Key key = lru_.back().first;
        auto& entry = lru_.back().second;
        TileCoord tc{key.first, key.second};
        if (dirty_.count(key)) {
            // Spill dirty tile to scratch (performance skeleton)
            std::vector<std::byte> raw(sizeof(Rgba8) * kTilePixels);
            std::memcpy(raw.data(), entry.px.data(), raw.size());
            scratch.spill(tc, raw);
        }
        index_.erase(key);
        dirty_.erase(key);
        lru_.pop_back();
    }
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

    // Performance skeleton: try fault from scratch on miss
    auto& perf = Performance::instance();
    auto& scratchDisk = perf.scratch();
    std::vector<std::byte> faulted;
    if (scratchDisk.fault(c, faulted) && faulted.size() == sizeof(Rgba8) * kTilePixels) {
        CachedTile tile;
        std::memcpy(tile.px.data(), faulted.data(), faulted.size());
        // ... (simplified, skip full for skeleton)
    }

    std::fill(scratch_.begin(), scratch_.end(), Rgbaf{});
    compositeStack(doc_.topLevelLayers(), c, scratch_, 0);

    CachedTile tile;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTilePixels); ++i) {
        // Display conversion is identity until color management (M6).
        tile.px[i] = toRgba8(scratch_[i]);
    }
    dirty_.erase(key);
    ++recompositeCount_;

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

void CanvasRenderer::onDocumentChanged(const Document&, const DocumentChange& change) {
    switch (change.kind) {
        case DocumentChange::Kind::Pixels:
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
            break;  // no visual change
    }
}

}  // namespace pe
