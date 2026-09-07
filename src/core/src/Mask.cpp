#include "pe/core/Mask.hpp"

#include "pe/core/Document.hpp"  // kMaxCanvasDimension

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

const MaskBuffer::GrayTile* MaskBuffer::findTile(TileCoord c) const noexcept {
    auto it = tiles_.find(keyOf(c));
    return it == tiles_.end() ? nullptr : &it->second;
}

void MaskBuffer::setTile(TileCoord c, const GrayTile& bytes) {
    tiles_[keyOf(c)] = bytes;
}

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

bool MaskBuffer::canTranslate(int dx, int dy, int64_t maxPixels) const {
    if ((dx == 0 && dy == 0) || tiles_.empty()) return true;

    const Rect src = contentBounds();
    if (src.isEmpty()) return true;

    // The destination must stay inside the coordinate range the rest of the engine is
    // bounded by; int64 throughout so the sums themselves cannot wrap. The bound is
    // inclusive: an edge landing exactly on the limit is representable.
    const int64_t limit = kMaxCanvasDimension;
    const int64_t dstLeft = static_cast<int64_t>(src.left()) + dx;
    const int64_t dstTop = static_cast<int64_t>(src.top()) + dy;
    const int64_t dstRight = static_cast<int64_t>(src.right()) + dx;
    const int64_t dstBottom = static_cast<int64_t>(src.bottom()) + dy;
    if (dstLeft < -limit || dstTop < -limit || dstRight > limit || dstBottom > limit) return false;

    // A whole-tile shift is a rekey, so it costs nothing per pixel and needs no budget.
    if (dx % kTileSize == 0 && dy % kTileSize == 0) return true;

    // contentBounds() is tile-granular, so a sparse mask can span a large box; the
    // general path walks that box, which is what the budget bounds.
    const int64_t area = static_cast<int64_t>(src.width) * static_cast<int64_t>(src.height);
    return area <= maxPixels;
}

bool MaskBuffer::translate(int dx, int dy, int64_t maxPixels) {
    if (!canTranslate(dx, dy, maxPixels)) return false;
    if ((dx == 0 && dy == 0) || tiles_.empty()) return true;

    const Rect src = contentBounds();
    if (src.isEmpty()) return true;

    // Tile-aligned shift: rekey the map, no per-pixel work.
    if (dx % kTileSize == 0 && dy % kTileSize == 0) {
        const int dcol = dx / kTileSize;
        const int drow = dy / kTileSize;
        std::map<Key, GrayTile> moved;
        for (auto& [key, tile] : tiles_) {
            moved.emplace(Key{key.first + dcol, key.second + drow}, tile);
        }
        tiles_ = std::move(moved);
        return true;
    }

    // General case: rebuild pixel by pixel. canTranslate already bounded this.
    MaskBuffer out;
    for (int y = src.top(); y < src.bottom(); ++y) {
        for (int x = src.left(); x < src.right(); ++x) {
            const uint8_t v = value(x, y);
            // Skip kOpaque so the result stays canonical: an absent tile already
            // reads kOpaque, so writing it would only allocate a redundant tile.
            if (v != kOpaque) out.setValue(x + dx, y + dy, v);
        }
    }
    tiles_ = std::move(out.tiles_);
    return true;
}

void MaskBuffer::compact(Rect region) noexcept {
    if (region.isEmpty()) return;
    // Only inspect tiles overlapping `region` so the work is bounded by the caller's edit, not the
    // whole buffer. Erase any that are entirely kOpaque (identical to absent; absent reads
    // kOpaque).
    const TileSpan span = tilesForRect(region);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            auto it = tiles_.find(keyOf(TileCoord{col, row}));
            if (it == tiles_.end()) continue;
            bool allOpaque = true;
            for (const uint8_t b : it->second) {
                if (b != kOpaque) {
                    allOpaque = false;
                    break;
                }
            }
            if (allOpaque) tiles_.erase(it);
        }
    }
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
