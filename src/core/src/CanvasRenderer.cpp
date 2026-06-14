#include "pe/core/CanvasRenderer.hpp"

#include "pe/core/Compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pe {

CanvasRenderer::CanvasRenderer(Document& doc) : doc_(doc) {
    doc_.addObserver(this);
}

CanvasRenderer::~CanvasRenderer() {
    doc_.removeObserver(this);
}

void CanvasRenderer::invalidate(Rect docRect) {
    if (docRect.isEmpty()) return;
    const TileSpan span = tilesForRect(docRect);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            dirty_.insert(keyOf(TileCoord{col, row}));
        }
    }
}

void CanvasRenderer::invalidateAll() noexcept {
    cache_.clear();
    dirty_.clear();
}

const CanvasRenderer::CachedTile& CanvasRenderer::ensureTile(TileCoord c) {
    const Key key = keyOf(c);
    auto it = cache_.find(key);
    const bool needComposite = (it == cache_.end()) || (dirty_.find(key) != dirty_.end());
    if (!needComposite) return it->second;

    std::vector<Rgbaf> acc(static_cast<std::size_t>(kTilePixels), Rgbaf{});
    compositeStack(doc_.topLevelLayers(), c, acc, 0);

    CachedTile tile;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTilePixels); ++i) {
        // Display conversion is identity until color management (M6); for now the
        // working space is sRGB/8-bit, so float -> Rgba8 is the display value.
        tile.px[i] = toRgba8(acc[i]);
    }
    dirty_.erase(key);
    ++recompositeCount_;
    it = cache_.insert_or_assign(key, std::move(tile)).first;
    return it->second;
}

PixelBuffer CanvasRenderer::renderRegion(Rect docRect) {
    if (docRect.isEmpty()) return PixelBuffer{};
    const int64_t area = static_cast<int64_t>(docRect.width) * static_cast<int64_t>(docRect.height);
    if (area > kMaxCompositeImagePixels) return PixelBuffer{};

    PixelBuffer out(docRect.width, docRect.height, Rgba8{});
    const TileSpan span = tilesForRect(docRect);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const CachedTile& tile = ensureTile(coord);
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(docRect);
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
            if (change.dirtyRegion.isEmpty()) {
                // Unknown extent (e.g. an empty layer added/removed): be safe.
                invalidateAll();
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
