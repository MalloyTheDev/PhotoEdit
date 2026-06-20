#include "pe/core/Mask.hpp"

#include "pe/core/Selection.hpp"

#include <cstddef>

namespace pe {

namespace {
// Bound per-fill iteration AND tile allocation (mirrors Selection's defenses).
// kMaxMaskTiles is the load-bearing bound; the pixel cap is defense-in-depth.
constexpr int64_t kMaxMaskTiles = 4096;
constexpr int64_t kMaxMaskPixels = 64'000'000;
constexpr int kCoordBound = 1 << 26;  // ~67M; keeps right()/bottom() in int

int localIndex(int coord) noexcept {
    int m = coord % kTileSize;
    if (m < 0) m += kTileSize;
    return m;
}

bool rejectFill(Rect r) noexcept {
    if (r.isEmpty()) return true;
    if (r.x < -kCoordBound || r.x > kCoordBound || r.y < -kCoordBound || r.y > kCoordBound ||
        r.width < 0 || r.width > 2 * kCoordBound || r.height < 0 || r.height > 2 * kCoordBound) {
        return true;
    }
    const TileSpan span = tilesForRect(r);
    const int64_t cols = static_cast<int64_t>(span.colEnd) - span.colBegin;
    const int64_t rows = static_cast<int64_t>(span.rowEnd) - span.rowBegin;
    if (cols <= 0 || rows <= 0) return true;
    if (cols * rows > kMaxMaskTiles) return true;
    return static_cast<int64_t>(r.width) * r.height > kMaxMaskPixels;
}
}  // namespace

uint8_t MaskBuffer::value(int x, int y) const noexcept {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = tiles_.find(keyOf(c));
    if (it == tiles_.end()) return kOpaque;  // absent -> fully revealing
    return it->second[static_cast<std::size_t>(localIndex(y)) * kTileSize +
                      static_cast<std::size_t>(localIndex(x))];
}

void MaskBuffer::setValue(int x, int y, uint8_t v) {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = tiles_.find(keyOf(c));
    if (it == tiles_.end()) {
        if (v == kOpaque) return;  // default; don't allocate a fully-revealing tile
        GrayTile tile;
        tile.fill(kOpaque);  // a new mask tile defaults to revealing
        it = tiles_.emplace(keyOf(c), tile).first;
    }
    it->second[static_cast<std::size_t>(localIndex(y)) * kTileSize +
               static_cast<std::size_t>(localIndex(x))] = v;
}

void MaskBuffer::fillRect(Rect r, uint8_t v) {
    if (rejectFill(r)) return;
    for (int y = r.top(); y < r.bottom(); ++y) {
        for (int x = r.left(); x < r.right(); ++x) {
            setValue(x, y, v);
        }
    }
}

Rect MaskBuffer::contentBounds() const noexcept {
    Rect bounds{};
    for (const auto& [key, tile] : tiles_) {
        (void)tile;
        bounds = bounds.united(tileBounds(TileCoord{key.first, key.second}));
    }
    return bounds;
}

bool maskFillFits(Rect canvas) noexcept {
    return !rejectFill(canvas);
}

Mask maskFromSelection(const Selection& selection, Rect canvas) {
    Mask mask(Mask::Kind::Layer);
    if (!selection.active()) return mask;  // all selected -> empty mask (reveal all)
    if (rejectFill(canvas)) return mask;
    for (int y = canvas.top(); y < canvas.bottom(); ++y) {
        for (int x = canvas.left(); x < canvas.right(); ++x) {
            mask.buffer().setValue(x, y, selection.value(x, y));
        }
    }
    return mask;
}

}  // namespace pe
