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

namespace {
// Working-area cap for the edge refinements. Tighter than the selection's 64 MP storage cap
// because these materialize several dense region buffers (mask + an int distance field, or two
// float blur buffers); 16 MP keeps the transient under ~256 MB. A larger region is a no-op.
constexpr int64_t kMaxRefinePixels = 16'000'000;
constexpr int kMaxRefineRadius = 30000;      // clamp grow/shrink radius (canvas-dimension scale)
constexpr float kMaxFeatherSigma = 1000.0f;  // clamp feather sigma
constexpr int kChamferOrtho = 3;             // 3-4 chamfer ~ Euclidean*3 (round, not boxy)
constexpr int kChamferDiag = 4;

[[nodiscard]] Rect expandRect(Rect r, int m) noexcept {
    return Rect{r.x - m, r.y - m, r.width + 2 * m, r.height + 2 * m};
}

// In-place chamfer distance transform + threshold on mask.r (0..255 coverage, treated as binary
// at 50%). grow==true: output 255 where the distance to the nearest IN pixel is <= radius (dilate);
// grow==false (shrink/erode): 255 where the distance to the nearest OUT pixel is > radius. The
// 3-4 chamfer approximates Euclidean distance scaled by kChamferOrtho, so the threshold is
// radius*kChamferOrtho. Two passes -> O(area).
void chamferThreshold(PixelBuffer& mask, int radius, bool grow) {
    const int w = mask.width();
    const int h = mask.height();
    constexpr int kInf = 1 << 28;  // +kChamferDiag stays well within int
    std::vector<int> dist(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), kInf);
    auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool in = mask.at(x, y).r >= 128;
            if (in == grow) dist[at(x, y)] = 0;  // seed: IN for grow, OUT for shrink
        }
    }
    for (int y = 0; y < h; ++y) {  // forward pass (uses already-computed up/left neighbors)
        for (int x = 0; x < w; ++x) {
            int d = dist[at(x, y)];
            if (x > 0) d = std::min(d, dist[at(x - 1, y)] + kChamferOrtho);
            if (y > 0) d = std::min(d, dist[at(x, y - 1)] + kChamferOrtho);
            if (x > 0 && y > 0) d = std::min(d, dist[at(x - 1, y - 1)] + kChamferDiag);
            if (x + 1 < w && y > 0) d = std::min(d, dist[at(x + 1, y - 1)] + kChamferDiag);
            dist[at(x, y)] = d;
        }
    }
    for (int y = h - 1; y >= 0; --y) {  // backward pass (down/right neighbors)
        for (int x = w - 1; x >= 0; --x) {
            int d = dist[at(x, y)];
            if (x + 1 < w) d = std::min(d, dist[at(x + 1, y)] + kChamferOrtho);
            if (y + 1 < h) d = std::min(d, dist[at(x, y + 1)] + kChamferOrtho);
            if (x + 1 < w && y + 1 < h) d = std::min(d, dist[at(x + 1, y + 1)] + kChamferDiag);
            if (x > 0 && y + 1 < h) d = std::min(d, dist[at(x - 1, y + 1)] + kChamferDiag);
            dist[at(x, y)] = d;
        }
    }
    const int thr = radius * kChamferOrtho;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool sel = grow ? (dist[at(x, y)] <= thr) : (dist[at(x, y)] > thr);
            const uint8_t v = sel ? 255 : 0;
            mask.set(x, y, Rgba8{v, v, v, 255});
        }
    }
}

// Separable Gaussian blur of mask.r (coverage 0..255). Samples past the buffer edge clamp-extend
// (replicate the edge value): feather clamps its region to the canvas, so this treats the canvas
// border as "the selection continues" — a selection touching/filling the canvas does NOT fade
// there (only a real selected/unselected boundary inside the buffer softens). sigma must be > 0.
void gaussianMask(PixelBuffer& mask, float sigma) {
    const int w = mask.width();
    const int h = mask.height();
    const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float k = std::exp(-static_cast<float>(i) * static_cast<float>(i) * inv2s2);
        kernel[static_cast<std::size_t>(i + radius)] = k;
        sum += k;
    }
    for (float& k : kernel) k /= sum;

    auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> src(n), tmp(n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) src[idx(x, y)] = static_cast<float>(mask.at(x, y).r);
    }
    for (int y = 0; y < h; ++y) {  // horizontal
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                const int sx = std::clamp(x + i, 0, w - 1);  // clamp-extend (replicate edge)
                acc += kernel[static_cast<std::size_t>(i + radius)] * src[idx(sx, y)];
            }
            tmp[idx(x, y)] = acc;
        }
    }
    for (int y = 0; y < h; ++y) {  // vertical, write back as uint8
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                const int sy = std::clamp(y + i, 0, h - 1);  // clamp-extend (replicate edge)
                acc += kernel[static_cast<std::size_t>(i + radius)] * tmp[idx(x, sy)];
            }
            const auto v =
                static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(acc)), 0, 255));
            mask.set(x, y, Rgba8{v, v, v, 255});
        }
    }
}
}  // namespace

void Selection::grow(int radius) {
    if (!active_ || radius <= 0) return;
    const int r = std::min(radius, kMaxRefineRadius);
    const Rect bounds = tightBounds();
    if (bounds.isEmpty()) return;
    // Expand by r so the dilation has room to grow into. NOT clamped to the canvas: the region
    // covers the whole selection (so off-canvas coverage round-trips and is preserved), and the
    // un-materialized exterior reads as unselected via toMask.
    const Rect region = expandRect(bounds, r);
    if (region.isEmpty()) return;
    if (static_cast<int64_t>(region.width) * region.height > kMaxRefinePixels) return;
    PixelBuffer mask = toMask(region);
    if (mask.isEmpty()) return;
    chamferThreshold(mask, r, /*grow=*/true);
    loadMask(mask, region.left(), region.top());
    if (tiles_.empty()) selectNone();
}

void Selection::shrink(int radius) {
    if (!active_ || radius <= 0) return;
    const int r = std::min(radius, kMaxRefineRadius);
    const Rect bounds = tightBounds();
    if (bounds.isEmpty()) return;
    // Expand by r (NOT clamped to the canvas) so the r-wide exterior collar is materialized as
    // unselected and seeds the erosion distance transform on EVERY side — including any edge that
    // coincides with the canvas boundary, so a canvas-filling selection still contracts inward.
    const Rect region = expandRect(bounds, r);
    if (region.isEmpty()) return;
    if (static_cast<int64_t>(region.width) * region.height > kMaxRefinePixels) return;
    PixelBuffer mask = toMask(region);
    if (mask.isEmpty()) return;
    chamferThreshold(mask, r, /*grow=*/false);
    loadMask(mask, region.left(), region.top());
    if (tiles_.empty()) selectNone();  // eroded to nothing -> deselect
}

void Selection::feather(float radius, Rect canvas) {
    if (!active_ || !(radius > 0.0f)) return;
    // Floor the sigma so 2*sigma*sigma can't underflow to 0 (which would make the Gaussian kernel
    // NaN); the UI already clamps, but the engine API must not produce garbage on a tiny input.
    const float sigma = std::clamp(radius, 0.05f, kMaxFeatherSigma);
    const int margin = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    const Rect bounds = tightBounds();
    if (bounds.isEmpty()) return;
    const Rect region = expandRect(bounds, margin).intersected(canvas);
    if (region.isEmpty()) return;
    if (static_cast<int64_t>(region.width) * region.height > kMaxRefinePixels) return;
    PixelBuffer mask = toMask(region);
    if (mask.isEmpty()) return;
    gaussianMask(mask, sigma);
    loadMask(mask, region.left(), region.top());
    if (tiles_.empty()) selectNone();
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
