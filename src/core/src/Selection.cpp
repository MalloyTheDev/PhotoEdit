#include "pe/core/Selection.hpp"

#include "pe/core/Orient.hpp"
#include "pe/core/Resample.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Diagnostics: coordinate-to-tile resolutions performed by the mask WRITE paths.
std::atomic<uint64_t> g_maskWriteLookups{0};
std::atomic<uint64_t> g_maskTileAllocs{0};

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

const Selection::GrayTile* Selection::findTile(TileCoord c) const noexcept {
    auto it = tiles_.find(keyOf(c));
    return it == tiles_.end() ? nullptr : &it->second;
}

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

template <class Src>
void Selection::setRun(int y, int xBegin, int xEnd, Src&& src) {
    if (xEnd <= xBegin) return;
    const TileCoord c{floorDiv(xBegin, kTileSize), floorDiv(y, kTileSize)};
    g_maskWriteLookups.fetch_add(1, std::memory_order_relaxed);
    auto it = tiles_.find(keyOf(c));  // ONE lookup for the whole run
    const std::size_t row = static_cast<std::size_t>(localIndex(y)) * kTileSize;
    const int lx0 = localIndex(xBegin);

    if (it != tiles_.end()) {  // present: write through, zeros included
        GrayTile& tile = it->second;
        int lx = lx0;
        for (int x = xBegin; x < xEnd; ++x, ++lx) {
            const std::size_t i = row + static_cast<std::size_t>(lx);
            tile[i] = src(x, y, tile[i]);
        }
        return;
    }
    // Absent: `current` is 0 across the whole run, so the values are known without touching
    // the map. setValue's rule, applied to a run: never allocate a tile to store only zeros.
    std::array<uint8_t, kTileSize> out{};
    const int n = xEnd - xBegin;
    bool anyNonZero = false;
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] = src(xBegin + i, y, static_cast<uint8_t>(0));
        anyNonZero = anyNonZero || out[static_cast<std::size_t>(i)] != 0;
    }
    if (!anyNonZero) return;
    g_maskTileAllocs.fetch_add(1, std::memory_order_relaxed);
    GrayTile& tile = tiles_.emplace(keyOf(c), GrayTile{}).first->second;
    std::copy_n(out.begin(), n,
                tile.begin() + static_cast<std::ptrdiff_t>(row) + static_cast<std::ptrdiff_t>(lx0));
}

template <class Src>
void Selection::forEachRun(Rect r, Src&& src) {
    if (r.isEmpty()) return;
    const int colBegin = floorDiv(r.left(), kTileSize);
    const int colEnd = floorDiv(r.right() - 1, kTileSize) + 1;
    for (int y = r.top(); y < r.bottom(); ++y) {
        for (int col = colBegin; col < colEnd; ++col) {
            const int runBegin = std::max(r.left(), col * kTileSize);
            const int runEnd = std::min(r.right(), (col + 1) * kTileSize);
            setRun(y, runBegin, runEnd, src);
        }
    }
}

void Selection::fillRect(Rect r, uint8_t v) {
    if (rejectFill(r)) return;
    forEachRun(r, [v](int, int, uint8_t) { return v; });
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

void Selection::writeMaskRegion(const PixelBuffer& mask, int originX, int originY) {
    // Replace coverage INSIDE the mask's region and leave everything outside it alone.
    // loadMask clears every tile first, which is right when the region provably covers the
    // whole selection (grow and shrink expand tightBounds, which is exact over non-zero
    // coverage) but destroys coverage when it does not. feather clamps its region to the
    // canvas on purpose, so it is the one caller whose region can be smaller.
    const Rect region{originX, originY, mask.width(), mask.height()};
    if (mask.isEmpty() || rejectFill(region)) return;
    forEachRun(region, [&mask, originX, originY](int x, int y, uint8_t) {
        return mask.at(x - originX, y - originY).r;
    });
    dropEmptyTiles();  // keep selectedBounds tight (don't retain all-zero tiles)
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
    forEachRun(region, [&mask, originX, originY](int x, int y, uint8_t) {
        return mask.at(x - originX, y - originY).r;
    });
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
    // An inactive selection means everything is selected (coverage() is 1.0 and
    // value() is 255 everywhere), so it inverts to nothing. Reading stored() here
    // instead saw 0 for every absent tile and inverted that to 255, which made
    // inverting Select All leave everything selected.
    const bool wasActive = active_;
    active_ = true;
    // One tile resolution serves both the read and the write of each run: `current` is the
    // stored coverage the run walk already has in hand, where the old code paid a separate
    // stored() lookup per pixel on top of the setValue one.
    forEachRun(canvas, [wasActive](int, int, uint8_t current) {
        const uint8_t cur = wasActive ? current : static_cast<uint8_t>(255);
        return static_cast<uint8_t>(255 - cur);
    });
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
    // Bound the kernel radius: the separable passes are O(w*h*radius), and with the feather sigma
    // clamped to 1000 the naive radius (~3*sigma) reaches ~3000, which over a 16 MP region is a
    // multi-second uninterruptible hang. Cap the work — the Gaussian tail past here is negligible.
    constexpr int kMaxGaussianRadius = 256;
    const int radius = std::clamp(static_cast<int>(std::ceil(sigma * 3.0f)), 1, kMaxGaussianRadius);
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
    // Funnel through rejectFill so the tile-count and coord-range caps apply too — a thin/extreme-
    // aspect region can pass the area cap yet exceed kMaxSelectionTiles, and loadMask would then
    // silently discard the whole selection. Plus the tighter refine area cap.
    if (rejectFill(region) ||
        static_cast<int64_t>(region.width) * region.height > kMaxRefinePixels) {
        return;
    }
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
    // Funnel through rejectFill so the tile-count and coord-range caps apply too — a thin/extreme-
    // aspect region can pass the area cap yet exceed kMaxSelectionTiles, and loadMask would then
    // silently discard the whole selection. Plus the tighter refine area cap.
    if (rejectFill(region) ||
        static_cast<int64_t>(region.width) * region.height > kMaxRefinePixels) {
        return;
    }
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
    // Funnel through rejectFill so the tile-count and coord-range caps apply too — a thin/extreme-
    // aspect region can pass the area cap yet exceed kMaxSelectionTiles, and loadMask would then
    // silently discard the whole selection. Plus the tighter refine area cap.
    if (rejectFill(region) ||
        static_cast<int64_t>(region.width) * region.height > kMaxRefinePixels) {
        return;
    }
    PixelBuffer mask = toMask(region);
    if (mask.isEmpty()) return;
    gaussianMask(mask, sigma);
    // NOT loadMask: `region` is clamped to the canvas, so anything selected outside the
    // canvas is not in `mask` and clearing first would delete it. grow/shrink can clear
    // because their region is unclamped and therefore covers the whole selection.
    writeMaskRegion(mask, region.left(), region.top());
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

SelectionOutline Selection::outline(std::uint8_t threshold, std::size_t maxSegments) const {
    SelectionOutline result;
    if (!active_) return result;
    const Rect b = tightBounds();
    if (b.isEmpty()) return result;

    // One pixel of margin on every side. Everything outside the selection is unselected, so
    // the margin is what makes the boundary of a run that reaches the edge of the bounds get
    // emitted, and it is what guarantees every vertical run is closed by the final row.
    const int x0 = b.left() - 1;
    const int x1 = b.right() + 1;  // exclusive
    const int y0 = b.top() - 1;
    const int y1 = b.bottom() + 1;  // exclusive
    const std::size_t w = static_cast<std::size_t>(x1 - x0);

    std::vector<unsigned char> prev(w, 0);
    std::vector<unsigned char> cur(w, 0);
    constexpr int kNone = std::numeric_limits<int>::min();
    std::vector<int> openY(w, kNone);  // per column: where its vertical run started

    // One tile lookup per tile per row, rather than one per pixel: the same reason
    // forEachRun exists on the write side.
    const auto readRow = [&](int y, std::vector<unsigned char>& dst) {
        std::fill(dst.begin(), dst.end(), 0);
        int x = x0;
        while (x < x1) {
            const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
            const int end = std::min((c.col + 1) * kTileSize, x1);
            if (const GrayTile* t = findTile(c); t != nullptr) {
                const std::size_t rowBase = static_cast<std::size_t>(localIndex(y)) * kTileSize;
                for (int gx = x; gx < end; ++gx) {
                    dst[static_cast<std::size_t>(gx - x0)] =
                        (*t)[rowBase + static_cast<std::size_t>(localIndex(gx))] >= threshold ? 1
                                                                                              : 0;
                }
            }
            x = end;
        }
    };

    for (int y = y0; y < y1; ++y) {
        readRow(y, cur);

        // Horizontal edges: between the row above and this one, merged along x.
        int runStart = kNone;
        for (std::size_t i = 0; i < w; ++i) {
            const bool edge = prev[i] != cur[i];
            if (edge && runStart == kNone) {
                runStart = x0 + static_cast<int>(i);
            } else if (!edge && runStart != kNone) {
                result.segments.push_back(
                    OutlineSegment{Point{runStart, y}, Point{x0 + static_cast<int>(i), y}});
                runStart = kNone;
            }
        }
        if (runStart != kNone) {
            result.segments.push_back(OutlineSegment{Point{runStart, y}, Point{x1, y}});
        }

        // Vertical edges: between neighbours in this row, accumulated down the columns.
        for (std::size_t i = 1; i < w; ++i) {
            const bool edge = cur[i - 1] != cur[i];
            if (edge && openY[i] == kNone) {
                openY[i] = y;
            } else if (!edge && openY[i] != kNone) {
                const int x = x0 + static_cast<int>(i);
                result.segments.push_back(OutlineSegment{Point{x, openY[i]}, Point{x, y}});
                openY[i] = kNone;
            }
        }

        if (result.segments.size() > maxSegments) {
            result.segments.clear();
            result.complete = false;
            return result;
        }
        std::swap(prev, cur);
    }
    // No run can still be open: the final row is the margin, which is entirely unselected,
    // so every column's edge state ended false and closed whatever it had.
    return result;
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

std::uint64_t maskWriteTileLookupCount() noexcept {
    return g_maskWriteLookups.load(std::memory_order_relaxed);
}

std::uint64_t maskTileAllocCount() noexcept {
    return g_maskTileAllocs.load(std::memory_order_relaxed);
}

Selection magicWandSelection(const PixelBuffer& image, int seedX, int seedY, int tolerance) {
    Selection sel;
    const int w = image.width();
    const int h = image.height();
    if (image.isEmpty() || seedX < 0 || seedX >= w || seedY < 0 || seedY >= h) return sel;
    // Bound the flood's working memory (visited + mask) the same way fills are bounded. rejectFill
    // adds the tile-count + coord-range caps on top of the pixel cap: an extreme aspect ratio (e.g.
    // 64M x 1) passes the pixel cap but exceeds kMaxSelectionTiles, so loadMask would discard the
    // result anyway — bail before running the whole O(pixels) flood + allocations.
    if (rejectFill(Rect{0, 0, w, h})) return sel;

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

Selection orientedSelection(const Selection& sel, Orient op, Size canvas) {
    if (!sel.active() || canvas.width <= 0 || canvas.height <= 0) return sel;
    const Rect bounds = sel.tightBounds();
    if (bounds.isEmpty()) return sel;
    const PixelBuffer mask = sel.toMask(bounds);
    if (mask.isEmpty()) return sel;  // over-cap: keep as-is rather than deactivate

    const Rect dst = orientRect(op, canvas, bounds);
    if (dst.isEmpty()) return sel;
    PixelBuffer out(dst.width, dst.height);
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            const Point s = orientInverse(op, canvas, Point{dst.left() + x, dst.top() + y});
            const int sx = s.x - bounds.left();
            const int sy = s.y - bounds.top();
            std::uint8_t v = 0;
            if (sx >= 0 && sx < mask.width() && sy >= 0 && sy < mask.height()) {
                v = mask.at(sx, sy).r;
            }
            out.set(x, y, Rgba8{v, v, v, 255});
        }
    }
    Selection res;
    res.loadMask(out, dst.left(), dst.top());
    return res;
}

Selection resampledSelection(const Selection& sel, Rect srcCanvas, Rect dstCanvas) {
    if (!sel.active() || srcCanvas.isEmpty() || dstCanvas.isEmpty()) return sel;
    const Rect bounds = sel.tightBounds();
    if (bounds.isEmpty()) return sel;  // active but no coverage

    const PixelBuffer mask = sel.toMask(bounds);
    // Over-cap: a sparse selection can have a huge bbox but tiny coverage, and toMask returns
    // empty rather than materialise it. Loading an empty mask would deactivate the selection, so
    // keep it as-is (same guard the crop's translatedSelection uses).
    if (mask.isEmpty()) return sel;

    // Scale about the document origin, by the canvas ratio, so the selection tracks the pixels.
    const double sx = static_cast<double>(dstCanvas.width) / static_cast<double>(srcCanvas.width);
    const double sy = static_cast<double>(dstCanvas.height) / static_cast<double>(srcCanvas.height);
    const int newX = static_cast<int>(std::lround(static_cast<double>(bounds.left()) * sx));
    const int newY = static_cast<int>(std::lround(static_cast<double>(bounds.top()) * sy));
    const int newW = static_cast<int>(std::lround(static_cast<double>(bounds.width) * sx));
    const int newH = static_cast<int>(std::lround(static_cast<double>(bounds.height) * sy));
    if (newW <= 0 || newH <= 0) return sel;  // rounded away to nothing: do not deactivate

    // Resample the coverage (the mask's red channel) and rebuild a coverage PixelBuffer.
    std::vector<std::uint8_t> cov(static_cast<std::size_t>(mask.width()) *
                                  static_cast<std::size_t>(mask.height()));
    for (int y = 0; y < mask.height(); ++y) {
        for (int x = 0; x < mask.width(); ++x) {
            cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.width()) +
                static_cast<std::size_t>(x)] = mask.at(x, y).r;
        }
    }
    const std::vector<std::uint8_t> scaled =
        resampleCoverage(cov, mask.width(), mask.height(), newW, newH);
    if (scaled.empty()) return sel;

    PixelBuffer scaledMask(newW, newH);
    for (int y = 0; y < newH; ++y) {
        for (int x = 0; x < newW; ++x) {
            const std::uint8_t v =
                scaled[static_cast<std::size_t>(y) * static_cast<std::size_t>(newW) +
                       static_cast<std::size_t>(x)];
            scaledMask.set(x, y, Rgba8{v, v, v, 255});
        }
    }
    Selection out;
    out.loadMask(scaledMask, newX, newY);
    return out;
}

}  // namespace pe
