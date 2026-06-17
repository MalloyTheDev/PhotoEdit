#include "pe/core/Selection.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

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

void Selection::selectPolygon(std::span<const Point> verts) {
    tiles_.clear();
    active_ = false;
    if (verts.size() < 3) return;  // a polygon needs at least three vertices

    int minX = verts[0].x;
    int maxX = verts[0].x;
    int minY = verts[0].y;
    int maxY = verts[0].y;
    for (const Point& p : verts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    // Reject extreme coordinates BEFORE computing the extent, so maxX-minX cannot overflow
    // int (and the edge interpolation stays in range). Matches coordsOutOfRange's bound.
    if (minX < -kCoordBound || maxX > kCoordBound || minY < -kCoordBound || maxY > kCoordBound) {
        return;
    }
    const Rect bbox{minX, minY, maxX - minX + 1, maxY - minY + 1};
    if (rejectFill(bbox)) return;  // empty / out-of-range / over the tile or pixel cap

    active_ = true;
    const std::size_t n = verts.size();
    std::vector<float> xs;
    for (int y = minY; y <= maxY; ++y) {
        const float yc = static_cast<float>(y) + 0.5f;  // sample at the pixel-row center
        xs.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const Point& a = verts[i];
            const Point& b = verts[(i + 1) % n];  // closing edge wraps to vertex 0
            const float ay = static_cast<float>(a.y);
            const float by = static_cast<float>(b.y);
            if ((ay <= yc && by > yc) || (by <= yc && ay > yc)) {  // edge crosses this row
                const float t = (yc - ay) / (by - ay);
                xs.push_back(static_cast<float>(a.x) + t * static_cast<float>(b.x - a.x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {  // even-odd: fill between pairs
            const int xStart = static_cast<int>(std::ceil(xs[k] - 0.5f));
            const int xEnd = static_cast<int>(std::floor(xs[k + 1] - 0.5f));
            for (int x = xStart; x <= xEnd; ++x) setValue(x, y, 255);
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

Selection magicWandSelection(const PixelBuffer& image, int seedX, int seedY, int tolerance) {
    Selection sel;
    const int w = image.width();
    const int h = image.height();
    if (image.isEmpty() || seedX < 0 || seedX >= w || seedY < 0 || seedY >= h) return sel;
    // Bound the flood's working memory (visited + mask) the same way fills are bounded.
    if (static_cast<int64_t>(w) * static_cast<int64_t>(h) > kMaxSelectionPixels) return sel;

    const int tol = std::clamp(tolerance, 0, 255);
    const Rgba8 seed = image.at(seedX, seedY);
    auto chDiff = [](uint8_t a, uint8_t b) { return a > b ? int(a) - int(b) : int(b) - int(a); };
    auto within = [&](Rgba8 c) {
        return chDiff(c.r, seed.r) <= tol && chDiff(c.g, seed.g) <= tol &&
               chDiff(c.b, seed.b) <= tol && chDiff(c.a, seed.a) <= tol;
    };

    std::vector<uint8_t> visited(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    std::vector<std::pair<int, int>> stack;
    PixelBuffer mask(w, h);  // zero-initialized: red channel 0 == not selected
    const auto seedIdx = static_cast<std::size_t>(seedY) * static_cast<std::size_t>(w) + seedX;
    visited[seedIdx] = 1;
    stack.emplace_back(seedX, seedY);
    bool any = false;
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        if (!within(image.at(x, y))) continue;  // pushed but out of tolerance: skip
        mask.set(x, y, Rgba8{255, 255, 255, 255});
        any = true;
        const int nbrs[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& nb : nbrs) {
            const int nx = nb[0];
            const int ny = nb[1];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            const auto i = static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + nx;
            if (visited[i]) continue;
            visited[i] = 1;
            stack.emplace_back(nx, ny);
        }
    }
    if (any) sel.loadMask(mask, 0, 0);  // 4-connected region as the new selection
    return sel;
}

}  // namespace pe
