#include "pe/core/Selection.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace pe {

namespace {
// Bound per-fill iteration AND tile allocation. The tile-count cap is the load-
// bearing one: a thin, enormous rect (e.g. 64M x 1) passes any area cap yet spans
// hundreds of thousands of tiles. Mirrors the brush's kMaxStrokeTiles defense.
constexpr int64_t kMaxSelectionPixels = 64'000'000;
constexpr int64_t kMaxSelectionTiles = 4096;  // ~256 MB worst case
constexpr int kCoordBound = 1 << 26;          // ~67M; keeps right()/bottom() in int

int localIndex(int coord) noexcept {
    int m = coord % kTileSize;
    if (m < 0) m += kTileSize;
    return m;
}

// Reject coordinates whose magnitude could overflow right()/bottom() or contains().
bool coordsOutOfRange(Rect r) noexcept {
    return r.x < -kCoordBound || r.x > kCoordBound || r.y < -kCoordBound || r.y > kCoordBound ||
           r.width < 0 || r.width > 2 * kCoordBound || r.height < 0 || r.height > 2 * kCoordBound;
}

// Reject a rect that is empty, out of range, or would allocate too many tiles.
bool rejectFill(Rect r) noexcept {
    if (r.isEmpty() || coordsOutOfRange(r)) return true;
    const TileSpan span = tilesForRect(r);
    const int64_t cols = static_cast<int64_t>(span.colEnd) - span.colBegin;
    const int64_t rows = static_cast<int64_t>(span.rowEnd) - span.rowBegin;
    if (cols <= 0 || rows <= 0) return true;
    if (cols * rows > kMaxSelectionTiles) return true;
    return static_cast<int64_t>(r.width) * r.height > kMaxSelectionPixels;
}
}  // namespace

uint8_t Selection::stored(int x, int y) const noexcept {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = tiles_.find(keyOf(c));
    if (it == tiles_.end()) return 0;
    return it->second[static_cast<std::size_t>(localIndex(y)) * kTileSize +
                      static_cast<std::size_t>(localIndex(x))];
}

void Selection::setValue(int x, int y, uint8_t v) {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = tiles_.find(keyOf(c));
    if (it == tiles_.end()) {
        if (v == 0) return;  // don't allocate a tile just to write transparent
        it = tiles_.emplace(keyOf(c), GrayTile{}).first;
    }
    it->second[static_cast<std::size_t>(localIndex(y)) * kTileSize +
               static_cast<std::size_t>(localIndex(x))] = v;
}

void Selection::fillRect(Rect r, uint8_t v) {
    if (rejectFill(r)) return;
    for (int y = r.top(); y < r.bottom(); ++y) {
        for (int x = r.left(); x < r.right(); ++x) {
            setValue(x, y, v);
        }
    }
}

void Selection::dropEmptyTiles() {
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        const GrayTile& t = it->second;
        const bool allZero = std::all_of(t.begin(), t.end(), [](uint8_t v) { return v == 0; });
        if (allZero) {
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
}

float Selection::coverage(int x, int y) const noexcept {
    if (!active_) return 1.0f;
    return static_cast<float>(stored(x, y)) / 255.0f;
}

uint8_t Selection::value(int x, int y) const noexcept {
    if (!active_) return 255;
    return stored(x, y);
}

void Selection::selectNone() noexcept {
    tiles_.clear();
    active_ = false;
}

PixelBuffer Selection::toMask(Rect bounds) const {
    // Reject empty, out-of-range (so bounds.left()+x can't overflow int), or oversized
    // bounds — same coordinate/area discipline as the fill paths.
    if (bounds.isEmpty() || coordsOutOfRange(bounds)) return PixelBuffer{};
    if (static_cast<int64_t>(bounds.width) * bounds.height > kMaxSelectionPixels) {
        return PixelBuffer{};
    }
    PixelBuffer out(bounds.width, bounds.height);
    for (int y = 0; y < bounds.height; ++y) {
        for (int x = 0; x < bounds.width; ++x) {
            const uint8_t v = value(bounds.left() + x, bounds.top() + y);
            out.set(x, y, Rgba8{v, v, v, 255});  // grayscale coverage, opaque
        }
    }
    return out;
}

void Selection::loadMask(const PixelBuffer& mask, int originX, int originY) {
    tiles_.clear();
    // Apply the same caps as the fill paths: empty, out-of-range origin (so originX+x
    // can't overflow int), or a mask too large to materialize -> select nothing.
    const Rect region{originX, originY, mask.width(), mask.height()};
    if (mask.isEmpty() || rejectFill(region)) {
        active_ = false;
        return;
    }
    active_ = true;
    for (int y = 0; y < mask.height(); ++y) {
        for (int x = 0; x < mask.width(); ++x) {
            setValue(originX + x, originY + y, mask.at(x, y).r);
        }
    }
    dropEmptyTiles();  // keep selectedBounds tight (don't retain all-zero tiles)
}

void Selection::selectAll(Rect canvas) {
    tiles_.clear();
    if (rejectFill(canvas)) {
        // Too large to materialize: leave inactive == fully editable (equivalent
        // to "all selected" for gating).
        active_ = false;
        return;
    }
    active_ = true;
    fillRect(canvas, 255);
}

void Selection::selectRect(Rect r) {
    tiles_.clear();
    if (rejectFill(r)) {
        active_ = false;
        return;
    }
    active_ = true;
    fillRect(r, 255);
}

void Selection::addRect(Rect r) {
    if (rejectFill(r)) return;  // validate BEFORE touching active_ (no empty-lockout)
    active_ = true;
    fillRect(r, 255);
}

void Selection::subtractRect(Rect r) {
    if (!active_ || coordsOutOfRange(r) || r.isEmpty()) return;
    // Operate per stored tile so cost is bounded by the selection, not by r.
    for (auto& [key, tile] : tiles_) {
        const Rect tb = tileBounds(TileCoord{key.first, key.second});
        const Rect hit = tb.intersected(r);
        if (hit.isEmpty()) continue;
        for (int y = hit.top(); y < hit.bottom(); ++y) {
            for (int x = hit.left(); x < hit.right(); ++x) {
                tile[static_cast<std::size_t>(localIndex(y)) * kTileSize +
                     static_cast<std::size_t>(localIndex(x))] = 0;
            }
        }
    }
    dropEmptyTiles();
}

void Selection::intersectRect(Rect r) {
    if (!active_) {
        // Intersecting an "all" selection with a rect selects that rect.
        if (!rejectFill(r)) selectRect(r);
        return;
    }
    if (coordsOutOfRange(r)) return;  // huge r: intersect keeps the current selection
    for (auto& [key, tile] : tiles_) {
        const Rect tb = tileBounds(TileCoord{key.first, key.second});
        for (int ly = 0; ly < kTileSize; ++ly) {
            for (int lx = 0; lx < kTileSize; ++lx) {
                if (!r.contains(Point{tb.left() + lx, tb.top() + ly})) {
                    tile[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] =
                        0;
                }
            }
        }
    }
    dropEmptyTiles();
}

void Selection::invert(Rect canvas) {
    if (rejectFill(canvas)) return;  // bounds the canvas iteration/allocation
    active_ = true;
    for (int y = canvas.top(); y < canvas.bottom(); ++y) {
        for (int x = canvas.left(); x < canvas.right(); ++x) {
            setValue(x, y, static_cast<uint8_t>(255 - stored(x, y)));
        }
    }
    dropEmptyTiles();
}

Rect Selection::selectedBounds() const noexcept {
    // tiles_ holds only non-all-zero tiles (dropEmptyTiles keeps it tight), so the
    // union of their bounds is a correct tile-granular selected bounds.
    Rect bounds{};
    for (const auto& [key, tile] : tiles_) {
        (void)tile;
        bounds = bounds.united(tileBounds(TileCoord{key.first, key.second}));
    }
    return bounds;
}

Rect Selection::tightBounds() const noexcept {
    // Exact pixel extent of the non-zero coverage. Bounded by kMaxSelectionTiles tiles, so
    // the worst-case scan is one-time work a caller does on selection change.
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();
    bool any = false;
    for (const auto& [key, tile] : tiles_) {
        const int baseX = key.first * kTileSize;
        const int baseY = key.second * kTileSize;
        for (int ly = 0; ly < kTileSize; ++ly) {
            const std::size_t row = static_cast<std::size_t>(ly) * kTileSize;
            for (int lx = 0; lx < kTileSize; ++lx) {
                if (tile[row + static_cast<std::size_t>(lx)] == 0) continue;
                any = true;
                const int gx = baseX + lx;
                const int gy = baseY + ly;
                minX = std::min(minX, gx);
                minY = std::min(minY, gy);
                maxX = std::max(maxX, gx);
                maxY = std::max(maxY, gy);
            }
        }
    }
    if (!any) return Rect{};
    return Rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

}  // namespace pe
