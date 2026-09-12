#include "pe/core/Filter.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/Orient.hpp"
#include "pe/core/Resample.hpp"

#include "pe/core/BlendMode.hpp"    // compositeOver (bucket fill)
#include "pe/core/Document.hpp"     // kMaxCanvasDimension
#include "pe/core/PixelBuffer.hpp"  // stampBuffer source raster
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"

#include <utility>
#include <vector>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace pe {

namespace {

inline std::size_t idx(int x, int y, int w) noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
}

inline int clampi(int v, int lo, int hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Separable 1D convolution along x (horizontal) with a clamped border.
void convolveH(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h,
               std::span<const float> kernel, int radius) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Rgbaf acc{};
            for (int j = -radius; j <= radius; ++j) {
                const float k = kernel[static_cast<std::size_t>(j + radius)];
                const Rgbaf& s = src[idx(clampi(x + j, 0, w - 1), y, w)];
                acc.r += k * s.r;
                acc.g += k * s.g;
                acc.b += k * s.b;
                acc.a += k * s.a;
            }
            dst[idx(x, y, w)] = acc;
        }
    }
}

// Separable 1D convolution along y (vertical) with a clamped border.
void convolveV(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h,
               std::span<const float> kernel, int radius) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Rgbaf acc{};
            for (int j = -radius; j <= radius; ++j) {
                const float k = kernel[static_cast<std::size_t>(j + radius)];
                const Rgbaf& s = src[idx(x, clampi(y + j, 0, h - 1), w)];
                acc.r += k * s.r;
                acc.g += k * s.g;
                acc.b += k * s.b;
                acc.a += k * s.a;
            }
            dst[idx(x, y, w)] = acc;
        }
    }
}

// Separable H+V convolution done in PREMULTIPLIED alpha, so transparent pixels
// (whose straight color is arbitrary) contribute zero color and don't bleed into
// opaque neighbors. Premultiply -> convolve -> unpremultiply.
void convolveSeparablePremult(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h,
                              std::span<const float> kernel, int radius) {
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<Rgbaf> pmul(n);
    std::vector<Rgbaf> tmp(n);
    for (std::size_t i = 0; i < n; ++i) pmul[i] = premultiply(src[i]);
    convolveH(pmul, tmp, w, h, kernel, radius);
    convolveV(tmp, dst, w, h, kernel, radius);
    for (std::size_t i = 0; i < n; ++i) dst[i] = unpremultiply(dst[i]);
}

void copyImage(std::span<const Rgbaf> src, std::span<Rgbaf> dst) {
    std::copy(src.begin(), src.end(), dst.begin());
}

}  // namespace

void boxBlur(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, int radius) {
    if (w <= 0 || h <= 0) return;
    if (radius <= 0) {
        copyImage(src, dst);
        return;
    }
    const float weight = 1.0f / static_cast<float>(2 * radius + 1);
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1), weight);
    convolveSeparablePremult(src, dst, w, h, kernel, radius);
}

void gaussianBlur(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, float sigma) {
    if (w <= 0 || h <= 0) return;
    // Reject non-positive AND non-finite sigma (the old `sigma <= 0` test let NaN/Inf through,
    // since NaN compares false — then `(int)ceil(3*sigma)` is an out-of-range float->int conversion
    // = UB).
    if (!(sigma > 0.0f) || !std::isfinite(sigma)) {
        copyImage(src, dst);
        return;
    }
    // Cap the kernel radius inside the engine (not just in the UI): an enormous finite sigma would
    // otherwise overflow `2*radius+1` and turn the separable convolution into an O(w*h*radius) DoS.
    // unsharpMask forwards its radius here as sigma, so this one cap covers both kernels.
    constexpr int kMaxKernelRadius = 1024;
    // Clamp in the FLOAT domain BEFORE the int cast: for a huge sigma, ceil(3*sigma) (e.g. 3e9)
    // is itself out of int range, so casting first — then clamping — would be the very UB we guard.
    const float wantRadius = std::ceil(3.0f * sigma);
    const int radius = wantRadius >= static_cast<float>(kMaxKernelRadius)
                           ? kMaxKernelRadius
                           : std::max(1, static_cast<int>(wantRadius));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    // Floor the sigma before squaring it. For a tiny positive value sigma*sigma
    // underflows to zero, so the exponent at i == 0 becomes -0.0f/0.0f = NaN, every
    // kernel entry normalizes to NaN, and the filtered region ends up fully
    // transparent on an integer layer or NaN-filled on a float one. Selection::feather
    // hit exactly this and carries the same clamp; this copy of the kernel builder
    // never got it. The floor is far below any visible blur, so it changes no result
    // that was previously correct.
    const float safeSigma = std::max(sigma, 0.05f);
    const float twoSigmaSq = 2.0f * safeSigma * safeSigma;
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float wgt = std::exp(-static_cast<float>(i * i) / twoSigmaSq);
        kernel[static_cast<std::size_t>(i + radius)] = wgt;
        sum += wgt;
    }
    for (float& k : kernel) k /= sum;  // normalize

    convolveSeparablePremult(src, dst, w, h, kernel, radius);
}

void unsharpMask(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, float radius,
                 float amount, float threshold) {
    if (w <= 0 || h <= 0) return;
    if (amount == 0.0f) {
        copyImage(src, dst);
        return;
    }
    std::vector<Rgbaf> blurred(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    gaussianBlur(src, blurred, w, h, radius);

    const auto sharpen = [&](float s, float b) {
        const float detail = s - b;
        // Always clamp on write (self-consistent output bound; matters once 16/32f
        // float output ships and the final 8-bit clamp no longer applies).
        if (std::fabs(detail) < threshold) return clamp01(s);
        return clamp01(s + amount * detail);
    };
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i) {
        const Rgbaf& s = src[i];
        const Rgbaf& b = blurred[i];
        dst[i] = Rgbaf{sharpen(s.r, b.r), sharpen(s.g, b.g), sharpen(s.b, b.b), clamp01(s.a)};
    }
}

void mosaic(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, int cell) {
    if (w <= 0 || h <= 0) return;
    if (cell <= 1) {
        copyImage(src, dst);
        return;
    }
    for (int by = 0; by < h; by += cell) {
        const int y1 = std::min(by + cell, h);
        for (int bx = 0; bx < w; bx += cell) {
            const int x1 = std::min(bx + cell, w);
            // Average in premultiplied alpha so transparent pixels add no color.
            Rgbaf sum{};
            int count = 0;
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    const Rgbaf pm = premultiply(src[idx(x, y, w)]);
                    sum.r += pm.r;
                    sum.g += pm.g;
                    sum.b += pm.b;
                    sum.a += pm.a;
                    ++count;
                }
            }
            const float inv = count > 0 ? 1.0f / static_cast<float>(count) : 0.0f;
            const Rgbaf avg =
                unpremultiply(Rgbaf{sum.r * inv, sum.g * inv, sum.b * inv, sum.a * inv});
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) dst[idx(x, y, w)] = avg;
            }
        }
    }
}

void medianFilter(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, int radius) {
    if (w <= 0 || h <= 0) return;
    if (radius <= 0) {
        copyImage(src, dst);
        return;
    }
    // Self-guard the radius here too (not only in the wrapper class): the cost grows
    // as r^2 and the window allocation is (2r+1)^2, so an unbounded radius from a
    // direct caller would overflow `int` and over-allocate. kMaxMedianRadius keeps
    // the window <= 31x31 and the arithmetic well within int range.
    constexpr int kMaxMedianRadius = 15;
    if (radius > kMaxMedianRadius) radius = kMaxMedianRadius;

    const int side = 2 * radius + 1;
    const std::size_t window = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    const std::size_t mid = window / 2;  // odd window -> middle element is the median
    std::vector<float> rs(window), gs(window), bs(window), as(window);
    const auto median = [mid](std::vector<float>& v) -> float {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
        return v[mid];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Clamping changes which source pixel is read, never the count, so the
            // window is always full — fill all `window` slots, overwriting last pixel.
            std::size_t n = 0;
            for (int j = -radius; j <= radius; ++j) {
                for (int i = -radius; i <= radius; ++i) {
                    const Rgbaf& s = src[idx(clampi(x + i, 0, w - 1), clampi(y + j, 0, h - 1), w)];
                    rs[n] = s.r;
                    gs[n] = s.g;
                    bs[n] = s.b;
                    as[n] = s.a;
                    ++n;
                }
            }
            dst[idx(x, y, w)] = Rgbaf{median(rs), median(gs), median(bs), median(as)};
        }
    }
}

void findEdges(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) {
    if (w <= 0 || h <= 0) return;
    // Sobel kernels: Gx detects horizontal gradients, Gy vertical. The output is the
    // inverted gradient magnitude per channel (flat -> white, edge -> dark).
    const auto at = [&](int x, int y) -> const Rgbaf& {
        return src[idx(clampi(x, 0, w - 1), clampi(y, 0, h - 1), w)];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgbaf tl = at(x - 1, y - 1), tc = at(x, y - 1), tr = at(x + 1, y - 1);
            const Rgbaf ml = at(x - 1, y), mr = at(x + 1, y);
            const Rgbaf bl = at(x - 1, y + 1), bc = at(x, y + 1), br = at(x + 1, y + 1);
            const auto edge = [](float a0, float a1, float a2, float a3, float a5, float a6,
                                 float a7, float a8) {
                const float gx = (a2 + 2.0f * a5 + a8) - (a0 + 2.0f * a3 + a6);
                const float gy = (a6 + 2.0f * a7 + a8) - (a0 + 2.0f * a1 + a2);
                return clamp01(1.0f - std::sqrt(gx * gx + gy * gy));
            };
            dst[idx(x, y, w)] =
                Rgbaf{edge(tl.r, tc.r, tr.r, ml.r, mr.r, bl.r, bc.r, br.r),
                      edge(tl.g, tc.g, tr.g, ml.g, mr.g, bl.g, bc.g, br.g),
                      edge(tl.b, tc.b, tr.b, ml.b, mr.b, bl.b, bc.b, br.b), clamp01(at(x, y).a)};
        }
    }
}

namespace {
// SplitMix32-style integer hash: maps a key to a well-mixed 32-bit value.
inline uint32_t hashU32(uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
// Uniform float in [0,1) from a hashed value's top 24 bits.
inline float uniform01(uint32_t h) noexcept {
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}
}  // namespace

void addNoise(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, float amount,
              bool monochromatic, bool gaussian, uint32_t seed) {
    if (w <= 0 || h <= 0) return;
    if (amount <= 0.0f) {
        copyImage(src, dst);
        return;
    }
    const float sigma = amount * 0.5f;  // Gaussian std-dev at full amount
    const uint32_t seedMix = seed * 0x9e3779b9U;
    // Deterministic, reproducible noise: derived purely from the pixel index, the
    // channel, and the seed (no global RNG state), so the same inputs always
    // produce the same output and it parallelizes trivially.
    const auto noiseDelta = [&](std::size_t i, int ch) -> float {
        const uint32_t key = (static_cast<uint32_t>(i) * 4u + static_cast<uint32_t>(ch)) ^ seedMix;
        if (gaussian) {
            const float u1 = std::max(uniform01(hashU32(key * 2u)), 1e-7f);  // avoid log(0)
            const float u2 = uniform01(hashU32(key * 2u + 1u));
            const float z =
                std::sqrt(-2.0f * std::log(u1)) * std::cos(6.28318530718f * u2);  // Box-Muller
            return z * sigma;
        }
        return (uniform01(hashU32(key)) * 2.0f - 1.0f) * amount;  // uniform [-amount,amount]
    };

    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i) {
        const Rgbaf& s = src[i];
        if (monochromatic) {
            const float d = noiseDelta(i, 0);  // same noise added to each channel
            dst[i] = Rgbaf{clamp01(s.r + d), clamp01(s.g + d), clamp01(s.b + d), clamp01(s.a)};
        } else {
            dst[i] = Rgbaf{clamp01(s.r + noiseDelta(i, 0)), clamp01(s.g + noiseDelta(i, 1)),
                           clamp01(s.b + noiseDelta(i, 2)), clamp01(s.a)};
        }
    }
}

namespace {
std::atomic<std::uint64_t> g_moveTileBuilds{0};

// Is every edge of `r` inside the engine's representable coordinate range? An empty rect is
// trivially fine: no record is emitted for one.
bool withinCoordinateRange(Rect r) noexcept {
    if (r.isEmpty()) return true;
    const std::int64_t lim = kMaxCanvasDimension;
    const std::int64_t x0 = r.x;
    const std::int64_t y0 = r.y;
    return x0 >= -lim && y0 >= -lim && x0 + r.width <= lim && y0 + r.height <= lim;
}

// Tiles `r` spans, as int64 so the product cannot overflow.
std::int64_t tileCountOf(Rect r) noexcept {
    if (r.isEmpty()) return 0;
    const TileSpan span = tilesForRect(r);
    return static_cast<std::int64_t>(span.colEnd - span.colBegin) *
           static_cast<std::int64_t>(span.rowEnd - span.rowBegin);
}

std::int64_t bytesPerPixelOf(BitDepth depth) noexcept {
    switch (depth) {
        case BitDepth::U16:
            return static_cast<std::int64_t>(sizeof(Rgba16));
        case BitDepth::F32:
            return static_cast<std::int64_t>(sizeof(Rgbaf));
        case BitDepth::U8:
        default:
            return static_cast<std::int64_t>(sizeof(Rgba8));
    }
}

// Depth-generic core of moveLayerContent: shift a layer's pixels by (dx, dy) at their
// NATIVE depth, one destination tile at a time.
//
// Why this exists rather than routing a Move through bakePixelEditImpl below. A Move is an
// integer translation: no resampling, no premultiplication, no blending. The generic bake
// reads the whole region into std::vector<Rgbaf>, copies it twice more and writes it back,
// which costs about 48 bytes per pixel of transient float. kMaxFilterPixels (16 MP) exists
// to bound exactly that (see the comment on it in Filter.hpp), so routing a Move through it
// meant paying a filter's memory bill to do a copy, and then being REFUSED on the strength
// of that bill: contentBounds() is tile-aligned, so a 4000x4000 canvas reports 16.78 MP and
// a Move on any photograph from a modern camera silently did nothing (#180).
//
// Here nothing region-sized is allocated. Each destination tile is built on its own, so
// peak transient memory is one tile, and the work is proportional to the tiles the move
// touches rather than to the region's area. That is what lifts the cap.
//
// Reads are run-based, not per-pixel: within one destination row the source row is fixed and
// the source columns are contiguous, so the source tile is resolved once per source tile
// column and the run is walked with a local index. Resolving per pixel is the access pattern
// #176 removed from the .pedoc writer, and it would be worse here.
template <class Pixel>
std::unique_ptr<PaintCommand> moveContentImpl(LayerId layerId, TileStoreT<Pixel>& store, Rect src,
                                              int dx, int dy, std::string name) {
    const Rect dst{src.x + dx, src.y + dy, src.width, src.height};
    // Both the vacated source and the destination change, so the edit spans their union.
    const Rect region = src.united(dst);

    std::vector<PaintCommand::DeltaT<Pixel>> deltas;
    Rect dirty{};
    const TileSpan span = tilesForRect(region);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(region);
            if (vis.isEmpty()) continue;
            // A tile touching neither the source nor the destination cannot change: it holds
            // no content to vacate (contentBounds bounds every occupied tile) and receives no
            // shifted pixel. Skipping it before the allocation below is what makes the cost
            // proportional to the CONTENT rather than to the bounding box of src and dst,
            // which for a long drag is mostly empty space between them.
            if (!tb.intersects(src) && !tb.intersects(dst)) continue;

            // Read the destination tile BEFORE any delta is applied. Nothing in this loop
            // mutates the store (sharedTile only arms the copy-on-write flag), so every read
            // below still sees the pre-move pixels, which is what makes reading the source
            // through the same store correct.
            g_moveTileBuilds.fetch_add(1, std::memory_order_relaxed);
            std::shared_ptr<TileDataT<Pixel>> before = store.sharedTile(coord);
            auto after = std::make_shared<TileDataT<Pixel>>();
            if (before) *after = *before;

            bool changed = false;
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const int sy = y - dy;
                const std::size_t rowBase =
                    static_cast<std::size_t>(y - tb.top()) * static_cast<std::size_t>(kTileSize);
                // The x range in this row whose source lies inside `src`. Everything outside
                // it is vacated and becomes transparent.
                // CLAMPED to the tile, both ends. The source span can lie wholly left or
                // wholly right of this tile, and an unclamped bound then sent the clearing
                // loops below straight past the tile buffer.
                const bool rowInSrc = sy >= src.top() && sy < src.bottom();
                const int xLo =
                    rowInSrc ? std::clamp(src.left() + dx, vis.left(), vis.right()) : vis.right();
                const int xHi =
                    rowInSrc ? std::clamp(src.right() + dx, vis.left(), vis.right()) : vis.right();

                const auto put = [&](int x, Pixel np) {
                    const std::size_t li = rowBase + static_cast<std::size_t>(x - tb.left());
                    if (!pixelEqual(np, after->px[li])) {
                        after->px[li] = np;
                        changed = true;
                    }
                };
                for (int x = vis.left(); x < xLo; ++x) put(x, Pixel{});
                for (int x = xHi; x < vis.right(); ++x) put(x, Pixel{});
                if (xLo >= xHi) continue;

                // The covered span, walked one source tile column at a time.
                const int syLocal = tileLocalOffset(sy);
                const int srcRow = floorDiv(sy, kTileSize);
                int x = xLo;
                while (x < xHi) {
                    const int sx = x - dx;
                    const int srcCol = floorDiv(sx, kTileSize);
                    // Last destination x still served by this source tile column.
                    const int runEnd = std::min(xHi, (srcCol + 1) * kTileSize + dx);
                    const TileDataT<Pixel>* stile = store.find(TileCoord{srcCol, srcRow});
                    if (stile == nullptr) {
                        for (; x < runEnd; ++x) put(x, Pixel{});  // absent source == clear
                        continue;
                    }
                    int lx = tileLocalOffset(sx);
                    for (; x < runEnd; ++x, ++lx) put(x, stile->at(lx, syLocal));
                }
            }
            if (changed) {
                // A tile the move emptied is removed, not stored as a transparent one.
                // setTile treats a null pointer as "erase", and DeltaT already documents that
                // a null `before` means the tile was absent, so undo restores it either way.
                // Storing it instead would leave contentBounds() permanently spanning the
                // vacated source as well as the destination, and since every destructive
                // filter is bounded by that rect, one Move could leave a layer unfilterable
                // over pixels that are all transparent.
                bool empty = true;
                for (const Pixel& px : after->px) {
                    if (!pixelEqual(px, Pixel{})) {
                        empty = false;
                        break;
                    }
                }
                deltas.push_back(PaintCommand::DeltaT<Pixel>{coord, std::move(before),
                                                             empty ? nullptr : std::move(after)});
                dirty = dirty.united(vis);
            }
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}

// Depth-generic core of transformLayerContent: resample a layer through `inv` (the DESTINATION
// to SOURCE map) one destination tile at a time, at the layer's native depth.
//
// Why this exists rather than routing a transform through bakePixelEditImpl. The generic bake
// reads the whole region into std::vector<Rgbaf> and copies it twice more, about 48 bytes per
// pixel of transient float, and kMaxFilterPixels (16 MP) exists to bound exactly that. Unlike
// a Move, a resample genuinely needs float and genuinely needs interpolated reads, so it
// cannot simply skip them. What it does NOT need is the whole region at once: a destination
// pixel reads a 2x2 neighbourhood of the SOURCE, so each destination tile can be built on its
// own straight out of the tile store.
//
// That is what lifts the cap. Free Transform used to decline in silence on any layer whose
// content bounds exceeded 16 MP, which is every photograph from a modern camera: the handles
// moved and the pixels did not (#180).
//
// Source reads go through a one-entry tile memo. The four bilinear taps of one destination
// pixel almost always land in the same source tile, and consecutive destination pixels land in
// the same tile as each other, so this turns four map lookups per pixel into roughly one per
// tile crossing. Resolving per tap is the access pattern #176 removed from the .pedoc writer
// and #168 removed from Selection, and it is not being reintroduced here.
template <class Pixel>
std::unique_ptr<PaintCommand> transformContentImpl(LayerId layerId, TileStoreT<Pixel>& store,
                                                   Rect src, Rect region, const Affine2D& inv,
                                                   std::string name) {
    // Nothing in this pass mutates the store, so a resolved tile pointer stays valid
    // throughout. sharedTile below only arms the copy-on-write flag.
    TileCoord memoCoord{std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    const TileDataT<Pixel>* memoTile = nullptr;
    const auto sourceAt = [&](int sx, int sy) -> Rgbaf {
        if (sx < src.left() || sx >= src.right() || sy < src.top() || sy >= src.bottom()) {
            return Rgbaf{};  // outside the content: transparent, never a stale colour
        }
        const TileCoord c{floorDiv(sx, kTileSize), floorDiv(sy, kTileSize)};
        if (c.col != memoCoord.col || c.row != memoCoord.row) {
            memoCoord = c;
            memoTile = store.find(c);
        }
        if (memoTile == nullptr) return Rgbaf{};
        return premultiply(toFloat(memoTile->at(tileLocalOffset(sx), tileLocalOffset(sy))));
    };

    // Bilinear in PREMULTIPLIED space, so the arbitrary colour of a transparent pixel cannot
    // bleed into its neighbours, then back to straight alpha. Identical arithmetic to the
    // region-based sampler this replaces; only where the taps come from has changed.
    const auto lerp = [](const Rgbaf& a, const Rgbaf& b, float t) {
        return Rgbaf{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                     a.a + (b.a - a.a) * t};
    };
    const auto sample = [&](double fx, double fy) -> Rgbaf {
        if (!std::isfinite(fx) || !std::isfinite(fy)) return Rgbaf{};
        const double flx = std::floor(fx);
        const double fly = std::floor(fy);
        // One pixel of slack on every side, so a destination pixel whose centre maps just
        // outside the content still receives its fractional edge tap and the anti-aliased
        // fringe survives. Also keeps the casts below in range.
        if (flx < static_cast<double>(src.left()) - 2.0 ||
            flx > static_cast<double>(src.right()) + 1.0 ||
            fly < static_cast<double>(src.top()) - 2.0 ||
            fly > static_cast<double>(src.bottom()) + 1.0) {
            return Rgbaf{};
        }
        const int x0 = static_cast<int>(flx);
        const int y0 = static_cast<int>(fly);
        const float tx = static_cast<float>(fx - flx);
        const float ty = static_cast<float>(fy - fly);
        const Rgbaf top = lerp(sourceAt(x0, y0), sourceAt(x0 + 1, y0), tx);
        const Rgbaf bot = lerp(sourceAt(x0, y0 + 1), sourceAt(x0 + 1, y0 + 1), tx);
        return unpremultiply(lerp(top, bot, ty));
    };

    std::vector<PaintCommand::DeltaT<Pixel>> deltas;
    Rect dirty{};
    const TileSpan span = tilesForRect(region);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(region);
            if (vis.isEmpty()) continue;

            std::shared_ptr<TileDataT<Pixel>> before = store.sharedTile(coord);
            auto after = std::make_shared<TileDataT<Pixel>>();
            if (before) *after = *before;

            bool changed = false;
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const std::size_t rowBase =
                    static_cast<std::size_t>(y - tb.top()) * static_cast<std::size_t>(kTileSize);
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const double ddx = static_cast<double>(x) + 0.5;  // pixel CENTRE
                    const double ddy = static_cast<double>(y) + 0.5;
                    const Pixel np = fromFloat<Pixel>(
                        sample(inv.applyX(ddx, ddy) - 0.5, inv.applyY(ddx, ddy) - 0.5));
                    const std::size_t li = rowBase + static_cast<std::size_t>(x - tb.left());
                    if (!pixelEqual(np, after->px[li])) {
                        after->px[li] = np;
                        changed = true;
                    }
                }
            }
            if (changed) {
                // An emptied tile is removed rather than stored transparent, for the same
                // reason as a Move: otherwise contentBounds permanently spans the vacated
                // source as well as the destination, and every filter is bounded by that.
                bool empty = true;
                for (const Pixel& px : after->px) {
                    if (!pixelEqual(px, Pixel{})) {
                        empty = false;
                        break;
                    }
                }
                deltas.push_back(PaintCommand::DeltaT<Pixel>{coord, std::move(before),
                                                             empty ? nullptr : std::move(after)});
                dirty = dirty.united(vis);
            }
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}

// Depth-generic core of bakePixelEdit: read the content rect from `store` at its
// native depth into a working-float image, run `transform`, then write the result
// back as native-depth tile deltas (with optional selection gating). One template
// per pixel type keeps the float math identical across depths.
template <class Pixel>
std::unique_ptr<PaintCommand> bakePixelEditImpl(
    LayerId layerId, TileStoreT<Pixel>& store, Rect bb, std::string name,
    const std::function<void(std::span<Rgbaf>, int, int)>& transform, const Selection* selection) {
    const int w = bb.width;
    const int h = bb.height;
    std::vector<Rgbaf> orig(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    // Walk the source tile by tile rather than in raster order. store.pixel() resolves
    // the tile through a map lookup on every call, and a raster walk re-resolves it for
    // every pixel; blocking by tile makes that one lookup per tile. Absent tiles read as
    // transparent, exactly as store.pixel() would report them.
    const TileSpan readSpan = tilesForRect(bb);
    for (int row = readSpan.rowBegin; row < readSpan.rowEnd; ++row) {
        for (int col = readSpan.colBegin; col < readSpan.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect vis = tileBounds(coord).intersected(bb);
            if (vis.isEmpty()) continue;
            const TileDataT<Pixel>* tile = store.find(coord);
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const Pixel p = tile != nullptr
                                        ? tile->at(tileLocalOffset(x), tileLocalOffset(y))
                                        : Pixel{};
                    orig[idx(x - bb.left(), y - bb.top(), w)] = toFloat(p);
                }
            }
        }
    }
    std::vector<Rgbaf> work = orig;  // the transform mutates this in place
    transform(std::span<Rgbaf>(work), w, h);

    const bool gate = selection != nullptr && selection->active();
    std::vector<PaintCommand::DeltaT<Pixel>> deltas;
    Rect dirty{};
    const TileSpan span = tilesForRect(bb);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(bb);
            if (vis.isEmpty()) continue;
            std::shared_ptr<TileDataT<Pixel>> before = store.sharedTile(coord);
            auto after = std::make_shared<TileDataT<Pixel>>();
            if (before) *after = *before;

            // One lookup for the selection tile instead of one per gated pixel; `gate`
            // already established active(), which is the precondition findTile carries.
            // Absent means coverage 0 for the whole tile, but the loop still runs: with
            // cov == 0 the lerp is not unconditionally the identity for a non-finite
            // working value, and this must stay bit-identical.
            const Selection::GrayTile* selTile = gate ? selection->findTile(coord) : nullptr;

            bool changed = false;
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const std::size_t si = idx(x - bb.left(), y - bb.top(), w);
                    Rgbaf out = work[si];
                    if (gate) {
                        const float cov =
                            selTile != nullptr
                                ? static_cast<float>(
                                      (*selTile)[static_cast<std::size_t>(tileLocalOffset(y)) *
                                                     kTileSize +
                                                 static_cast<std::size_t>(tileLocalOffset(x))]) /
                                      255.0f
                                : 0.0f;
                        const Rgbaf& o = orig[si];
                        out.r = o.r + (out.r - o.r) * cov;
                        out.g = o.g + (out.g - o.g) * cov;
                        out.b = o.b + (out.b - o.b) * cov;
                        out.a = o.a + (out.a - o.a) * cov;
                    }
                    const Pixel np = fromFloat<Pixel>(out);
                    const std::size_t li = static_cast<std::size_t>(y - tb.top()) * kTileSize +
                                           static_cast<std::size_t>(x - tb.left());
                    if (!pixelEqual(np, after->px[li])) {
                        after->px[li] = np;
                        changed = true;
                    }
                }
            }
            if (changed) {
                deltas.push_back(
                    PaintCommand::DeltaT<Pixel>{coord, std::move(before), std::move(after)});
                dirty = dirty.united(vis);
            }
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}
}  // namespace

Refusal bakeRefusal(const Document& doc, LayerId layerId) {
    // Mirrors bakePixelEditRegion's preconditions, in the same order, using the same
    // constants. The region a filter passes is the layer's content bounds, so that is what
    // is measured here.
    const Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr) {
        return refuse("bake", RefusalCode::NoActiveLayer, {}, "Select a layer to apply this to.");
    }
    const std::string named = "\"" + layer->name() + "\"";
    if (layer->kind() != LayerKind::Pixel) {
        return refuse("bake", RefusalCode::LayerNotPixel, {},
                      named +
                          " is not a pixel layer. Select a pixel layer, or add an adjustment "
                          "layer instead.");
    }
    const Rect bb = static_cast<const PixelLayer*>(layer)->contentBounds();
    if (bb.isEmpty()) {
        return refuse("bake", RefusalCode::NoEffect, {},
                      named + " is empty, so there is nothing to change.");
    }
    if (bb.width > kMaxCanvasDimension || bb.height > kMaxCanvasDimension ||
        bb.x > kMaxCanvasDimension || bb.x < -kMaxCanvasDimension || bb.y > kMaxCanvasDimension ||
        bb.y < -kMaxCanvasDimension) {
        return refuse("bake", RefusalCode::OverSizeBudget, {},
                      named + " extends beyond the coordinate range the engine can edit.");
    }
    const int64_t area = static_cast<int64_t>(bb.width) * static_cast<int64_t>(bb.height);
    if (area > kMaxFilterPixels) {
        return refuse("bake", RefusalCode::OverSizeBudget, {},
                      named + " covers " + std::to_string(area / 1'000'000) +
                          " megapixels. Applying this is limited to " +
                          std::to_string(kMaxFilterPixels / 1'000'000) +
                          " megapixels; select a smaller region first.",
                      "content " + std::to_string(bb.width) + "x" + std::to_string(bb.height));
    }
    return Refusal{};  // nothing stops it
}

Refusal moveRefusal(const Document& doc, LayerId layerId, int dx, int dy) {
    // Mirrors moveLayerContent's guards, in the same order, using the same constants.
    if (dx == 0 && dy == 0) {
        return refuse("layer.move", RefusalCode::NoEffect, {},
                      "The layer is already where it started.");
    }
    const Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr) {
        return refuse("layer.move", RefusalCode::NoActiveLayer, {}, "Select a layer to move.");
    }
    const std::string named = "\"" + layer->name() + "\"";
    if (layer->kind() != LayerKind::Pixel) {
        return refuse("layer.move", RefusalCode::LayerNotPixel, {},
                      named + " is not a pixel layer, so its pixels cannot be moved.");
    }
    if (dx > kMaxCanvasDimension || dx < -kMaxCanvasDimension || dy > kMaxCanvasDimension ||
        dy < -kMaxCanvasDimension) {
        return refuse("layer.move", RefusalCode::OverSizeBudget, {},
                      "That is farther than one move can shift content.");
    }
    const auto* pl = static_cast<const PixelLayer*>(layer);
    const Rect src = pl->contentBounds();
    if (src.isEmpty()) {
        return refuse("layer.move", RefusalCode::NoEffect, {},
                      named + " is empty, so there is nothing to move.");
    }
    const Rect dst{src.x + dx, src.y + dy, src.width, src.height};
    if (!withinCoordinateRange(src) || !withinCoordinateRange(dst)) {
        return refuse("layer.move", RefusalCode::OverSizeBudget, {},
                      named + " would end up beyond the coordinate range the engine can store.");
    }
    const std::int64_t bytes = (tileCountOf(src) + tileCountOf(dst)) *
                               static_cast<std::int64_t>(kTilePixels) *
                               bytesPerPixelOf(pl->depth());
    if (bytes > kMaxMoveBytes) {
        return refuse("layer.move", RefusalCode::OverSizeBudget, {},
                      named + " would need " + std::to_string(bytes / (1024 * 1024)) +
                          " MB to move and undo, over the " +
                          std::to_string(kMaxMoveBytes / (1024 * 1024)) +
                          " MB limit. Move a smaller layer, or flatten first.",
                      "content " + std::to_string(src.width) + "x" + std::to_string(src.height) +
                          " at " + std::to_string(bytesPerPixelOf(pl->depth())) + " bytes/px");
    }
    return Refusal{};  // nothing stops it
}

std::unique_ptr<PaintCommand> bakePixelEditRegion(
    Document& doc, LayerId layerId, std::string name, Rect region,
    const std::function<void(std::span<Rgbaf>, int, int)>& transform, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);

    const Rect bb = region;
    if (bb.isEmpty()) return nullptr;
    if (bb.width > kMaxCanvasDimension || bb.height > kMaxCanvasDimension) return nullptr;
    // Bound the origin too, so right()/bottom() (computed as int x+width downstream, e.g. in
    // tilesForRect) cannot overflow for a far-off-canvas region. With both |x|,|y| and width,
    // height <= kMaxCanvasDimension, the sums stay well within int. (Canvas-bounded callers pass
    // |x|,|y| <= the canvas size; this only rejects pathological external origins.)
    if (bb.x > kMaxCanvasDimension || bb.x < -kMaxCanvasDimension || bb.y > kMaxCanvasDimension ||
        bb.y < -kMaxCanvasDimension) {
        return nullptr;
    }
    const int64_t area = static_cast<int64_t>(bb.width) * static_cast<int64_t>(bb.height);
    if (area > kMaxFilterPixels) return nullptr;

    // Edit the layer's pixels at their native storage depth; the float transform is
    // identical across depths (the high-depth stores avoid the 8-bit round-trip).
    switch (pl->depth()) {
        case BitDepth::U16:
            return bakePixelEditImpl<Rgba16>(layerId, pl->tiles16(), bb, std::move(name), transform,
                                             selection);
        case BitDepth::F32:
            return bakePixelEditImpl<Rgbaf>(layerId, pl->tilesF(), bb, std::move(name), transform,
                                            selection);
        case BitDepth::U8:
        default:
            return bakePixelEditImpl<Rgba8>(layerId, pl->tiles(), bb, std::move(name), transform,
                                            selection);
    }
}

std::unique_ptr<PaintCommand> bakePixelEdit(
    Document& doc, LayerId layerId, std::string name,
    const std::function<void(std::span<Rgbaf>, int, int)>& transform, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    // Filters/adjustments act on the layer's existing content (the region never grows).
    return bakePixelEditRegion(doc, layerId, std::move(name),
                               static_cast<PixelLayer*>(layer)->contentBounds(), transform,
                               selection);
}

std::uint64_t moveTileBuildCount() noexcept {
    return g_moveTileBuilds.load(std::memory_order_relaxed);
}

std::unique_ptr<PaintCommand> moveLayerContent(Document& doc, LayerId layerId, int dx, int dy) {
    if (dx == 0 && dy == 0) return nullptr;  // no movement, nothing to commit
    // Bound the offset so src+offset cannot overflow int and a pathological drag is rejected
    // (you cannot shift content farther than a canvas dimension in one command).
    if (dx > kMaxCanvasDimension || dx < -kMaxCanvasDimension || dy > kMaxCanvasDimension ||
        dy < -kMaxCanvasDimension) {
        return nullptr;
    }
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);
    const Rect src = pl->contentBounds();
    if (src.isEmpty()) return nullptr;  // empty layer: nothing to move

    // Validate the SOURCE rect, not just the offset. The generic bake used to do this on the
    // way past, and its comment explains why it matters: Rect::right() is x + width in int,
    // so an origin or far edge outside the engine's coordinate range overflows on first use,
    // including inside tilesForRect just below. Moving off that path took the check with it
    // and left only the offset bound, which is not enough on its own.
    const Rect dst{src.x + dx, src.y + dy, src.width, src.height};
    if (!withinCoordinateRange(src) || !withinCoordinateRange(dst)) return nullptr;

    // Bounded by the tiles the move actually TOUCHES, and by their bytes.
    //
    // Not by the region's area: kMaxFilterPixels exists to bound a filter's several
    // full-region float buffers, and a translation allocates none of them, which is why it
    // used to refuse an ordinary photograph outright (#180).
    //
    // Not by the bounding box of src and dst either: a small layer dragged a long way spans
    // a huge box that is almost entirely empty, and charging for that refuses the drag for
    // its distance rather than for its content.
    //
    // And counted in BYTES rather than tiles, because a Move's tiles are the layer's own
    // pixels: 256 KB at U8 but 1 MB at F32. A flat tile count would have let a one-pixel
    // nudge on a large float layer build a multi-gigabyte undo record, four times what the
    // same count costs at 8 bit.
    const std::int64_t srcTiles = tileCountOf(src);
    const std::int64_t dstTiles = tileCountOf(dst);
    const std::int64_t bytesPerTile =
        static_cast<std::int64_t>(kTilePixels) * bytesPerPixelOf(pl->depth());
    if ((srcTiles + dstTiles) * bytesPerTile > kMaxMoveBytes) return nullptr;

    // Native depth, no float round trip: a translation is a copy, so it neither needs the
    // working-float image nor should pay for it.
    switch (pl->depth()) {
        case BitDepth::U16:
            return moveContentImpl<Rgba16>(layerId, pl->tiles16(), src, dx, dy, "Move");
        case BitDepth::F32:
            return moveContentImpl<Rgbaf>(layerId, pl->tilesF(), src, dx, dy, "Move");
        case BitDepth::U8:
        default:
            return moveContentImpl<Rgba8>(layerId, pl->tiles(), src, dx, dy, "Move");
    }
}

std::unique_ptr<PaintCommand> transformLayerContent(Document& doc, LayerId layerId,
                                                    const Affine2D& srcToDst,
                                                    Rect regionOfInterest) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    const Rect src = static_cast<PixelLayer*>(layer)->contentBounds();
    if (src.isEmpty()) return nullptr;  // nothing to transform

    // Reject a singular or non-finite transform (would be non-invertible / collapse to nothing).
    const double det = srcToDst.determinant();
    if (!std::isfinite(det) || std::fabs(det) < 1e-9) return nullptr;

    constexpr double kBound = static_cast<double>(kMaxCanvasDimension);

    // Lossless fast path: a pure INTEGER translation needs no resampling, so reuse the exact
    // (byte-identical, premultiply-free) Move path. This keeps an integer move bit-exact and makes
    // identity (dx == dy == 0) a true no-op (moveLayerContent returns nullptr) regardless of any
    // fully-transparent-but-colored pixels — which the resampling path below would otherwise
    // normalize to {0,0,0,0}.
    if (srcToDst.m00 == 1.0 && srcToDst.m11 == 1.0 && srcToDst.m01 == 0.0 && srcToDst.m10 == 0.0 &&
        std::floor(srcToDst.m02) == srcToDst.m02 && std::floor(srcToDst.m12) == srcToDst.m12 &&
        std::fabs(srcToDst.m02) <= kBound && std::fabs(srcToDst.m12) <= kBound) {
        // Not narrowed by the region of interest: a translation is a copy rather than a
        // resample, so it is already cheap enough not to need one, and keeping it whole
        // preserves the bit-exactness that is the whole reason for this path.
        return moveLayerContent(doc, layerId, static_cast<int>(srcToDst.m02),
                                static_cast<int>(srcToDst.m12));
    }

    // Destination bounding box = the transformed source corners. Validate finiteness and range
    // before the double->int cast (an absurd transform must reject, never produce UB).
    const double cx[4] = {static_cast<double>(src.left()), static_cast<double>(src.right()),
                          static_cast<double>(src.left()), static_cast<double>(src.right())};
    const double cy[4] = {static_cast<double>(src.top()), static_cast<double>(src.top()),
                          static_cast<double>(src.bottom()), static_cast<double>(src.bottom())};
    double minX = kBound;
    double minY = kBound;
    double maxX = -kBound;
    double maxY = -kBound;
    for (int i = 0; i < 4; ++i) {
        const double dx = srcToDst.applyX(cx[i], cy[i]);
        const double dy = srcToDst.applyY(cx[i], cy[i]);
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > kBound ||
            std::fabs(dy) > kBound) {
            return nullptr;  // off-canvas / overflow: refuse rather than clamp the result
        }
        minX = std::min(minX, dx);
        minY = std::min(minY, dy);
        maxX = std::max(maxX, dx);
        maxY = std::max(maxY, dy);
    }
    // Pad by 1px on every side: a destination pixel whose center maps just outside the source still
    // gets a fractional bilinear tap from the edge, so the anti-aliased fringe lives one pixel
    // beyond the rounded corner bbox. bakePixelEditRegion still caps the area, and the sampler
    // returns transparent for the extra ring, so this only recovers the AA ramp (no over-draw).
    const int dl = static_cast<int>(std::floor(minX)) - 1;
    const int dt = static_cast<int>(std::floor(minY)) - 1;
    const int dr = static_cast<int>(std::ceil(maxX)) + 1;
    const int db = static_cast<int>(std::ceil(maxY)) + 1;
    const Rect dst{dl, dt, dr - dl, db - dt};
    if (dst.isEmpty()) return nullptr;

    // The edit spans both the vacated source and the destination: clear the old pixels and
    // write the resampled ones in one reversible in-region transform.
    Rect region = src.united(dst);
    if (!withinCoordinateRange(region)) return nullptr;
    // An empty intersection needs no special case: it spans no tiles, so the resample below
    // produces no deltas and returns nullptr on its own, which is what a drag whose whole
    // effect is off screen should hand back.
    if (!regionOfInterest.isEmpty()) region = region.intersected(regionOfInterest);

    // Bounded in BYTES by the tiles it touches, exactly as a Move is, rather than by
    // kMaxFilterPixels. The resample builds one destination tile at a time out of the store,
    // so it allocates nothing region-sized and the filter budget never applied to it. That
    // budget is what made Free Transform decline in silence on any photograph (#180).
    auto* pl = static_cast<PixelLayer*>(layer);
    const std::int64_t bytes = (tileCountOf(src) + tileCountOf(dst)) *
                               static_cast<std::int64_t>(kTilePixels) *
                               bytesPerPixelOf(pl->depth());
    if (bytes > kMaxMoveBytes) return nullptr;

    const Affine2D inv = srcToDst.inverted();
    switch (pl->depth()) {
        case BitDepth::U16:
            return transformContentImpl<Rgba16>(layerId, pl->tiles16(), src, region, inv,
                                                "Transform");
        case BitDepth::F32:
            return transformContentImpl<Rgbaf>(layerId, pl->tilesF(), src, region, inv,
                                               "Transform");
        case BitDepth::U8:
        default:
            return transformContentImpl<Rgba8>(layerId, pl->tiles(), src, region, inv, "Transform");
    }
}

// ---- Image Size: a separable-scale resample of one pixel layer ------------------------------

namespace {

// Resample a pixel layer's content sampled over `srcCanvas` onto `dstCanvas`, as a reversible
// tile-delta command. This is the axis-aligned-scale cousin of transformContentImpl: a scale is
// separable (unlike the rotation transformContentImpl must handle), so each output pixel is a
// 1-D horizontal combine of source columns then a 1-D vertical combine of the results, with the
// Catmull-Rom weights precomputed once per axis (buildResampleAxis) and shared with the
// contiguous resampleImage this is validated against.
//
// The sampling domain is the CANVAS, not the layer's tile-aligned contentBounds: clamp-to-edge
// therefore replicates the canvas-edge pixel of a canvas-filling image (no transparent rim),
// while a smaller layer's own edge still fades into the transparency around it (premultiplied).
// The rewritten region is contentBounds united with dstCanvas, so old content outside the new
// canvas is cleared rather than left behind at the old scale.
//
// Still tile-streamed: the horizontal pass builds an intermediate only for the columns and
// source-row band the current destination tile needs, so nothing region-sized is allocated.
template <class Pixel>
std::unique_ptr<PaintCommand> resampleContentImpl(LayerId layerId, TileStoreT<Pixel>& store,
                                                  Rect srcCanvas, Rect dstCanvas,
                                                  std::string name) {
    const ResampleAxis ax = buildResampleAxis(srcCanvas.width, dstCanvas.width);
    const ResampleAxis ay = buildResampleAxis(srcCanvas.height, dstCanvas.height);
    if (ax.empty() || ay.empty()) return nullptr;

    const Rect region = store.contentBounds().united(dstCanvas);

    // Premultiplied, clamp-to-edge source read with a one-entry tile memo. Indices are
    // canvas-LOCAL (0-based within srcCanvas); clamp-to-edge reads the edge column/row for a tap
    // past the canvas border. An absent tile inside the canvas still reads transparent.
    TileCoord memoCoord{INT_MIN, INT_MIN};
    const TileDataT<Pixel>* memoTile = nullptr;
    const auto srcPremul = [&](int scLocal, int srLocal) -> Rgbaf {
        const int sc =
            scLocal < 0 ? 0 : (scLocal >= srcCanvas.width ? srcCanvas.width - 1 : scLocal);
        const int sr =
            srLocal < 0 ? 0 : (srLocal >= srcCanvas.height ? srcCanvas.height - 1 : srLocal);
        const int docx = srcCanvas.x + sc;
        const int docy = srcCanvas.y + sr;
        const TileCoord c{floorDiv(docx, kTileSize), floorDiv(docy, kTileSize)};
        if (c.col != memoCoord.col || c.row != memoCoord.row) {
            memoCoord = c;
            memoTile = store.find(c);
        }
        if (memoTile == nullptr) return Rgbaf{};
        return premultiply(toFloat(memoTile->at(tileLocalOffset(docx), tileLocalOffset(docy))));
    };

    std::vector<PaintCommand::DeltaT<Pixel>> deltas;
    Rect dirty{};
    std::vector<Rgbaf> inter;  // per-tile horizontal-pass buffer, reused across tiles
    const TileSpan span = tilesForRect(region);
    for (int trow = span.rowBegin; trow < span.rowEnd; ++trow) {
        for (int tcol = span.colBegin; tcol < span.colEnd; ++tcol) {
            const TileCoord coord{tcol, trow};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(region);
            if (vis.isEmpty()) continue;

            std::shared_ptr<TileDataT<Pixel>> before = store.sharedTile(coord);
            auto after = std::make_shared<TileDataT<Pixel>>();
            if (before) *after = *before;
            bool changed = false;

            // Pixels of this tile outside the new canvas clear to transparent: the vacated
            // source, and any old content beyond the resized canvas.
            const Rect visInDst = vis.intersected(dstCanvas);
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const std::size_t rowBase =
                    static_cast<std::size_t>(y - tb.top()) * static_cast<std::size_t>(kTileSize);
                const bool rowInDst =
                    visInDst.height > 0 && y >= visInDst.top() && y < visInDst.bottom();
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const bool inDst = rowInDst && x >= visInDst.left() && x < visInDst.right();
                    if (inDst) continue;  // written by the resample pass below
                    const std::size_t li = rowBase + static_cast<std::size_t>(x - tb.left());
                    if (!pixelEqual(after->px[li], Pixel{})) {
                        after->px[li] = Pixel{};
                        changed = true;
                    }
                }
            }

            if (!visInDst.isEmpty()) {
                const int ox0 = visInDst.left() - dstCanvas.x;
                const int ox1 = visInDst.right() - dstCanvas.x;
                const int oy0 = visInDst.top() - dstCanvas.y;
                const int oy1 = visInDst.bottom() - dstCanvas.y;
                const int cols = ox1 - ox0;

                int syLo = INT_MAX;
                int syHi = INT_MIN;
                for (int oy = oy0; oy < oy1; ++oy) {
                    const int f = ay.first[static_cast<std::size_t>(oy)];
                    if (f < syLo) syLo = f;
                    if (f + ay.support - 1 > syHi) syHi = f + ay.support - 1;
                }
                const int bandRows = syHi - syLo + 1;

                inter.assign(static_cast<std::size_t>(cols) * static_cast<std::size_t>(bandRows),
                             Rgbaf{0.0f, 0.0f, 0.0f, 0.0f});
                for (int sr = syLo; sr <= syHi; ++sr) {
                    const std::size_t interRow =
                        static_cast<std::size_t>(sr - syLo) * static_cast<std::size_t>(cols);
                    for (int ox = ox0; ox < ox1; ++ox) {
                        const int first = ax.first[static_cast<std::size_t>(ox)];
                        const std::size_t wb =
                            static_cast<std::size_t>(ox) * static_cast<std::size_t>(ax.support);
                        Rgbaf acc{0.0f, 0.0f, 0.0f, 0.0f};
                        for (int t = 0; t < ax.support; ++t) {
                            const float w = ax.weights[wb + static_cast<std::size_t>(t)];
                            if (w == 0.0f) continue;
                            const Rgbaf pp = srcPremul(first + t, sr);
                            acc.r += pp.r * w;
                            acc.g += pp.g * w;
                            acc.b += pp.b * w;
                            acc.a += pp.a * w;
                        }
                        inter[interRow + static_cast<std::size_t>(ox - ox0)] = acc;
                    }
                }

                for (int oy = oy0; oy < oy1; ++oy) {
                    const int first = ay.first[static_cast<std::size_t>(oy)];
                    const std::size_t wb =
                        static_cast<std::size_t>(oy) * static_cast<std::size_t>(ay.support);
                    const int docy = dstCanvas.y + oy;
                    const std::size_t rowBase = static_cast<std::size_t>(docy - tb.top()) *
                                                static_cast<std::size_t>(kTileSize);
                    for (int ox = ox0; ox < ox1; ++ox) {
                        Rgbaf acc{0.0f, 0.0f, 0.0f, 0.0f};
                        for (int t = 0; t < ay.support; ++t) {
                            const float w = ay.weights[wb + static_cast<std::size_t>(t)];
                            if (w == 0.0f) continue;
                            const Rgbaf& pp = inter[static_cast<std::size_t>(first + t - syLo) *
                                                        static_cast<std::size_t>(cols) +
                                                    static_cast<std::size_t>(ox - ox0)];
                            acc.r += pp.r * w;
                            acc.g += pp.g * w;
                            acc.b += pp.b * w;
                            acc.a += pp.a * w;
                        }
                        Rgbaf straight;
                        if (acc.a <= 0.0f) {
                            straight = Rgbaf{0.0f, 0.0f, 0.0f, 0.0f};
                        } else {
                            const float inv = 1.0f / acc.a;
                            const float oa = acc.a > 1.0f ? 1.0f : acc.a;
                            straight = Rgbaf{acc.r * inv, acc.g * inv, acc.b * inv, oa};
                        }
                        const Pixel np = fromFloat<Pixel>(straight);
                        const std::size_t li =
                            rowBase + static_cast<std::size_t>(dstCanvas.x + ox - tb.left());
                        if (!pixelEqual(np, after->px[li])) {
                            after->px[li] = np;
                            changed = true;
                        }
                    }
                }
            }

            if (changed) {
                bool empty = true;
                for (const Pixel& px : after->px) {
                    if (!pixelEqual(px, Pixel{})) {
                        empty = false;
                        break;
                    }
                }
                deltas.push_back(PaintCommand::DeltaT<Pixel>{coord, std::move(before),
                                                             empty ? nullptr : std::move(after)});
                dirty = dirty.united(vis);
            }
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}

}  // namespace

Refusal resampleRefusal(const Document& doc, LayerId layerId, Rect srcCanvas, Rect dstCanvas) {
    // Mirrors resampleLayerContent's guards, in the same order, using the same constants.
    const Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr) {
        return refuse("image.size", RefusalCode::NoActiveLayer, {}, "Select a layer.");
    }
    const std::string named = "\"" + layer->name() + "\"";
    if (layer->kind() != LayerKind::Pixel) {
        return refuse("image.size", RefusalCode::LayerNotPixel, {},
                      named + " is not a pixel layer.");
    }
    if (srcCanvas.isEmpty() || dstCanvas.isEmpty()) {
        return refuse("image.size", RefusalCode::NoEffect, {}, "The canvas is empty.");
    }
    const auto* pl = static_cast<const PixelLayer*>(layer);
    if (pl->contentBounds().isEmpty()) return Refusal{};  // nothing to resample; not a blocker
    const Rect region = pl->contentBounds().united(dstCanvas);
    if (!withinCoordinateRange(region)) {
        return refuse("image.size", RefusalCode::OverSizeBudget, {},
                      named + " would exceed the coordinate range the engine can store.");
    }
    const std::int64_t bytes =
        tileCountOf(region) * static_cast<std::int64_t>(kTilePixels) * bytesPerPixelOf(pl->depth());
    if (bytes > kMaxMoveBytes) {
        return refuse("image.size", RefusalCode::OverSizeBudget, {},
                      named + " would need " + std::to_string(bytes / (1024 * 1024)) +
                          " MB to resample and undo, over the " +
                          std::to_string(kMaxMoveBytes / (1024 * 1024)) +
                          " MB limit. Resize a smaller document, or flatten first.",
                      "region " + std::to_string(region.width) + "x" +
                          std::to_string(region.height) + " at " +
                          std::to_string(bytesPerPixelOf(pl->depth())) + " bytes/px");
    }
    return Refusal{};  // nothing stops it
}

std::unique_ptr<PaintCommand> resampleLayerContent(Document& doc, LayerId layerId, Rect srcCanvas,
                                                   Rect dstCanvas) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (srcCanvas.isEmpty() || dstCanvas.isEmpty()) return nullptr;

    auto* pl = static_cast<PixelLayer*>(layer);
    if (pl->contentBounds().isEmpty()) return nullptr;  // nothing to resample

    const Rect region = pl->contentBounds().united(dstCanvas);
    if (!withinCoordinateRange(region)) return nullptr;

    // Bounded in bytes by the tiles it touches, like a Move and unlike a filter: it builds one
    // destination tile at a time, so kMaxFilterPixels never applies and a photograph does not
    // silently decline (#180).
    const std::int64_t bytes =
        tileCountOf(region) * static_cast<std::int64_t>(kTilePixels) * bytesPerPixelOf(pl->depth());
    if (bytes > kMaxMoveBytes) return nullptr;

    switch (pl->depth()) {
        case BitDepth::U16:
            return resampleContentImpl<Rgba16>(layerId, pl->tiles16(), srcCanvas, dstCanvas,
                                               "Image Size");
        case BitDepth::F32:
            return resampleContentImpl<Rgbaf>(layerId, pl->tilesF(), srcCanvas, dstCanvas,
                                              "Image Size");
        case BitDepth::U8:
        default:
            return resampleContentImpl<Rgba8>(layerId, pl->tiles(), srcCanvas, dstCanvas,
                                              "Image Size");
    }
}

// ---- Image Rotation: an exact reorientation of one pixel layer ------------------------------

namespace {

// Reorient a pixel layer's content by an exact permutation about the canvas. Each destination
// pixel is the source at orientInverse(op, canvas, dest); a destination whose source is empty
// reads transparent, so the vacated area clears. The rewritten region is the content bounds united
// with where they map to, so both the old and new positions are covered. No float, no premultiply:
// a 90/180/flip is lossless, so this copies bytes verbatim.
template <class Pixel>
std::unique_ptr<PaintCommand> orientContentImpl(LayerId layerId, TileStoreT<Pixel>& store,
                                                Orient op, Size canvas, std::string name) {
    const Rect content = store.contentBounds();
    if (content.isEmpty()) return nullptr;
    const Rect region = content.united(orientRect(op, canvas, content));

    std::vector<PaintCommand::DeltaT<Pixel>> deltas;
    Rect dirty{};
    const TileSpan span = tilesForRect(region);
    for (int trow = span.rowBegin; trow < span.rowEnd; ++trow) {
        for (int tcol = span.colBegin; tcol < span.colEnd; ++tcol) {
            const TileCoord coord{tcol, trow};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(region);
            if (vis.isEmpty()) continue;

            std::shared_ptr<TileDataT<Pixel>> before = store.sharedTile(coord);
            auto after = std::make_shared<TileDataT<Pixel>>();
            if (before) *after = *before;
            bool changed = false;

            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const std::size_t rowBase =
                    static_cast<std::size_t>(y - tb.top()) * static_cast<std::size_t>(kTileSize);
                for (int x = vis.left(); x < vis.right(); ++x) {
                    // Read the ORIGINAL store (unmutated until the command executes), so in-tile
                    // permutations (a flip mapping within one tile) never read a half-written tile.
                    const Point s = orientInverse(op, canvas, Point{x, y});
                    const Pixel np = store.pixel(s.x, s.y);
                    const std::size_t li = rowBase + static_cast<std::size_t>(x - tb.left());
                    if (!pixelEqual(after->px[li], np)) {
                        after->px[li] = np;
                        changed = true;
                    }
                }
            }

            if (changed) {
                bool empty = true;
                for (const Pixel& px : after->px) {
                    if (!pixelEqual(px, Pixel{})) {
                        empty = false;
                        break;
                    }
                }
                deltas.push_back(PaintCommand::DeltaT<Pixel>{coord, std::move(before),
                                                             empty ? nullptr : std::move(after)});
                dirty = dirty.united(vis);
            }
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
}

}  // namespace

std::unique_ptr<PaintCommand> orientLayerContent(Document& doc, LayerId layerId, Orient op,
                                                 std::string name) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);
    if (pl->contentBounds().isEmpty()) return nullptr;

    const Size canvas = doc.canvasSize();
    const Rect region = pl->contentBounds().united(orientRect(op, canvas, pl->contentBounds()));
    if (!withinCoordinateRange(region)) return nullptr;
    const std::int64_t bytes =
        tileCountOf(region) * static_cast<std::int64_t>(kTilePixels) * bytesPerPixelOf(pl->depth());
    if (bytes > kMaxMoveBytes) return nullptr;

    switch (pl->depth()) {
        case BitDepth::U16:
            return orientContentImpl<Rgba16>(layerId, pl->tiles16(), op, canvas, std::move(name));
        case BitDepth::F32:
            return orientContentImpl<Rgbaf>(layerId, pl->tilesF(), op, canvas, std::move(name));
        case BitDepth::U8:
        default:
            return orientContentImpl<Rgba8>(layerId, pl->tiles(), op, canvas, std::move(name));
    }
}

Refusal orientRefusal(const Document& doc, LayerId layerId, Orient op) {
    // Mirrors orientLayerContent's guards, in the same order, using the same constants.
    const Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr) {
        return refuse("image.rotate", RefusalCode::NoActiveLayer, {}, "Select a layer.");
    }
    const std::string named = "\"" + layer->name() + "\"";
    if (layer->kind() != LayerKind::Pixel) {
        return refuse("image.rotate", RefusalCode::LayerNotPixel, {},
                      named + " is not a pixel layer.");
    }
    const auto* pl = static_cast<const PixelLayer*>(layer);
    if (pl->contentBounds().isEmpty()) return Refusal{};  // nothing to reorient; not a blocker
    const Size canvas = doc.canvasSize();
    const Rect region = pl->contentBounds().united(orientRect(op, canvas, pl->contentBounds()));
    if (!withinCoordinateRange(region)) {
        return refuse("image.rotate", RefusalCode::OverSizeBudget, {},
                      named + " would exceed the coordinate range the engine can store.");
    }
    const std::int64_t bytes =
        tileCountOf(region) * static_cast<std::int64_t>(kTilePixels) * bytesPerPixelOf(pl->depth());
    if (bytes > kMaxMoveBytes) {
        return refuse("image.rotate", RefusalCode::OverSizeBudget, {},
                      named + " would need " + std::to_string(bytes / (1024 * 1024)) +
                          " MB to reorient and undo, over the " +
                          std::to_string(kMaxMoveBytes / (1024 * 1024)) + " MB limit.");
    }
    return Refusal{};
}

std::unique_ptr<PaintCommand> applyFilter(Document& doc, LayerId layerId, const Filter& filter,
                                          const Selection* selection) {
    return bakePixelEdit(
        doc, layerId, filter.displayName(),
        [&](std::span<Rgbaf> img, int w, int h) {
            std::vector<Rgbaf> in(img.begin(), img.end());  // filters are out-of-place
            filter.apply(in, img, w, h);
        },
        selection);
}

std::unique_ptr<PaintCommand> bucketFill(Document& doc, LayerId layerId, int seedX, int seedY,
                                         Rgbaf fillColor, int tolerance,
                                         const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    // Fill over the canvas region so transparent areas fill too; the flood is bounded by it.
    const Rect region = doc.canvasBounds();
    if (region.isEmpty() || !region.contains(Point{seedX, seedY})) return nullptr;
    const float tolF = static_cast<float>(std::clamp(tolerance, 0, 255)) / 255.0f;
    return bakePixelEditRegion(
        doc, layerId, "Fill", region,
        [seedX, seedY, fillColor, tolF, region](std::span<Rgbaf> img, int w, int h) {
            const std::vector<Rgbaf> orig(img.begin(), img.end());  // match against the original
            const int sx = seedX - region.x;
            const int sy = seedY - region.y;
            const Rgbaf seed = orig[idx(sx, sy, w)];
            auto chDiff = [](float a, float b) { return a > b ? a - b : b - a; };
            auto within = [&](const Rgbaf& c) {
                return chDiff(c.r, seed.r) <= tolF && chDiff(c.g, seed.g) <= tolF &&
                       chDiff(c.b, seed.b) <= tolF && chDiff(c.a, seed.a) <= tolF;
            };
            std::vector<uint8_t> visited(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                                         0);
            std::vector<std::pair<int, int>> stack;
            visited[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + sx] = 1;
            stack.emplace_back(sx, sy);
            while (!stack.empty()) {
                const auto [x, y] = stack.back();
                stack.pop_back();
                if (!within(orig[idx(x, y, w)])) continue;  // pushed but out of tolerance
                img[idx(x, y, w)] =
                    compositeOver(BlendMode::Normal, orig[idx(x, y, w)], fillColor, 1.0f);
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
        },
        selection);
}

// ---- Region copy / clear ----

Rect copyRegionFor(const Document& doc, const Selection* selection) {
    const Rect canvas = doc.canvasBounds();
    if (selection == nullptr || !selection->active()) return canvas;
    // tightBounds, not selectedBounds: the latter snaps out to whole tiles, so copying a small
    // selection would drag along up to 255 pixels of its neighbours on every side.
    return selection->tightBounds().intersected(canvas);
}

void applySelectionAlpha(PixelBuffer& img, Point origin, const Selection* selection) {
    if (img.isEmpty() || selection == nullptr || !selection->active()) return;
    Rgba8* p = img.data();
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const float cov = selection->coverage(origin.x + x, origin.y + y);
            Rgba8& px = p[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width()) +
                          static_cast<std::size_t>(x)];
            const float a = static_cast<float>(px.a) * cov;
            px.a = static_cast<std::uint8_t>(std::clamp(std::lround(a), 0L, 255L));
        }
    }
}

PixelBuffer copyLayerRegion(const Document& doc, LayerId layerId, Rect region,
                            const Selection* selection) {
    const Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return PixelBuffer{};
    if (region.isEmpty()) return PixelBuffer{};
    // Caps before the allocation, as everywhere else in this file: a region is caller-supplied
    // and a bad one must be refused rather than sized into a multi-hundred-MB buffer first.
    if (region.width > kMaxCanvasDimension || region.height > kMaxCanvasDimension) {
        return PixelBuffer{};
    }
    if (static_cast<int64_t>(region.width) * static_cast<int64_t>(region.height) >
        kMaxFilterPixels) {
        return PixelBuffer{};
    }
    const auto* pl = static_cast<const PixelLayer*>(layer);
    PixelBuffer out(region.width, region.height);
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            out.set(x, y, pl->tiles().pixel(region.x + x, region.y + y));
        }
    }
    applySelectionAlpha(out, Point{region.x, region.y}, selection);
    return out;
}

std::unique_ptr<PaintCommand> clearRegion(Document& doc, LayerId layerId, Rect region,
                                          const Selection* selection) {
    if (region.isEmpty()) return nullptr;
    return bakePixelEditRegion(
        doc, layerId, "Clear", region,
        [](std::span<Rgbaf> img, int, int) {
            // Every channel, not only alpha. A pixel left with its colour and zero alpha is
            // invisible but not empty, and a later operation that raises alpha again (or an
            // export that ignores it) would bring the old colour back.
            for (Rgbaf& px : img) px = Rgbaf{0.0f, 0.0f, 0.0f, 0.0f};
        },
        selection);
}

std::unique_ptr<PixelLayer> layerFromBuffer(const PixelBuffer& src, Point origin, std::string name,
                                            const Selection* selectionMask) {
    if (src.isEmpty()) return nullptr;
    if (src.width() > kMaxCanvasDimension || src.height() > kMaxCanvasDimension) return nullptr;
    if (static_cast<int64_t>(src.width()) * static_cast<int64_t>(src.height()) > kMaxFilterPixels) {
        return nullptr;
    }
    auto layer = std::make_unique<PixelLayer>(std::move(name));
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            const Rgba8 px = src.at(x, y);
            // Skip fully transparent pixels rather than writing them: the tile store is sparse,
            // and materializing a tile per empty region would make a paste of a small shape cost
            // as much as a paste of its whole bounding box.
            if (px.a != 0) layer->tiles().setPixel(origin.x + x, origin.y + y, px);
        }
    }
    if (selectionMask != nullptr && selectionMask->active()) {
        auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
        const Rect bounds{origin.x, origin.y, src.width(), src.height()};
        for (int y = 0; y < bounds.height; ++y) {
            for (int x = 0; x < bounds.width; ++x) {
                const std::uint8_t v = selectionMask->value(bounds.x + x, bounds.y + y);
                // An ABSENT mask pixel reads as kOpaque, so it is the revealing value that is
                // free and the hiding one that has to be written. Skipping the zeros instead
                // would produce a mask that reveals everything, which is no mask at all.
                if (v != MaskBuffer::kOpaque) {
                    mask->buffer().setValue(bounds.x + x, bounds.y + y, v);
                }
            }
        }
        layer->setMask(std::move(mask));
    }
    return layer;
}

std::unique_ptr<PaintCommand> gradientFill(Document& doc, LayerId layerId, Point start, Point end,
                                           Rgbaf c0, Rgbaf c1, const Selection* selection) {
    // One implementation. A two-colour gradient is a two-stop ramp, and the colours are already
    // literal, so nothing is substituted into it.
    return gradientFill(doc, layerId, start, end, Gradient::twoStop(c0, c1), c0, c1, selection);
}

std::unique_ptr<PaintCommand> gradientFill(Document& doc, LayerId layerId, Point start, Point end,
                                           const Gradient& gradient, Rgbaf foreground,
                                           Rgbaf background, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    const double dx = static_cast<double>(end.x) - start.x;
    const double dy = static_cast<double>(end.y) - start.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 < 1.0) return nullptr;  // zero-/sub-pixel-length drag: no gradient
    const Rect region = doc.canvasBounds();
    if (region.isEmpty()) return nullptr;
    return bakePixelEditRegion(
        doc, layerId, "Gradient", region,
        [start, dx, dy, len2, &gradient, foreground, background, region](std::span<Rgbaf> img,
                                                                         int w, int h) {
            const std::vector<Rgbaf> orig(img.begin(),
                                          img.end());  // composite the gradient over these
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    // Project the pixel center onto the start->end axis, clamped to [0,1].
                    const double px = region.x + x + 0.5 - start.x;
                    const double py = region.y + y + 0.5 - start.y;
                    double t = (px * dx + py * dy) / len2;
                    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                    const Rgbaf stop =
                        gradient.sample(static_cast<float>(t), foreground, background);
                    // Composite straight-alpha (Normal) over the backdrop, like bucketFill: a
                    // semi-transparent stop lets existing pixels show through, and compositeOver
                    // clamps every channel (sinking NaN / bounding range) at any layer depth.
                    img[idx(x, y, w)] =
                        compositeOver(BlendMode::Normal, orig[idx(x, y, w)], stop, 1.0f);
                }
            }
        },
        selection);
}

std::unique_ptr<PaintCommand> stampBuffer(Document& doc, LayerId layerId, Point origin,
                                          const PixelBuffer& src, std::string name,
                                          const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    if (src.isEmpty()) return nullptr;
    // Enforce the size caps BEFORE the float snapshot allocation (the engine's "DoS caps before
    // allocation" rule): bakePixelEditRegion re-checks, but it runs only after srcF is built, so a
    // huge source would otherwise force a multi-hundred-MB transient before being rejected.
    if (src.width() > kMaxCanvasDimension || src.height() > kMaxCanvasDimension) return nullptr;
    if (static_cast<int64_t>(src.width()) * static_cast<int64_t>(src.height()) > kMaxFilterPixels) {
        return nullptr;
    }
    const Rect region{origin.x, origin.y, src.width(), src.height()};
    // Snapshot the source as working-float, index-aligned with the region the transform sees
    // (row-major, w == region.width == src.width). The origin is bounded by bakePixelEditRegion.
    std::vector<Rgbaf> srcF(static_cast<std::size_t>(src.width()) *
                            static_cast<std::size_t>(src.height()));
    for (std::size_t i = 0; i < srcF.size(); ++i) srcF[i] = toFloat(src.data()[i]);
    return bakePixelEditRegion(
        doc, layerId, std::move(name), region,
        [srcF = std::move(srcF)](std::span<Rgbaf> img, int w, int h) {
            const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
            for (std::size_t i = 0; i < n; ++i) {
                img[i] = compositeOver(BlendMode::Normal, img[i], srcF[i], 1.0f);
            }
        },
        selection);
}

}  // namespace pe
