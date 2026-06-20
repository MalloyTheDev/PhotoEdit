#include "pe/core/Filter.hpp"

#include "pe/core/BlendMode.hpp"    // compositeOver (bucket fill)
#include "pe/core/Document.hpp"     // kMaxCanvasDimension
#include "pe/core/PixelBuffer.hpp"  // stampBuffer source raster
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"

#include <utility>
#include <vector>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pe {

namespace {

// Max content area a destructive filter rasterizes at once. Tighter than the
// compositor's 8-bit cap because filters allocate several full-region FLOAT
// buffers (premultiplied src, tmp, dst, blurred) — ~16 bytes/px each. 16 MP keeps
// transient memory ~1 GB. Larger regions need tile-streaming (a later increment).
constexpr int64_t kMaxFilterPixels = 16'000'000;

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
    const float twoSigmaSq = 2.0f * sigma * sigma;
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
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            orig[idx(x, y, w)] = toFloat(store.pixel(bb.left() + x, bb.top() + y));
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

            bool changed = false;
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const std::size_t si = idx(x - bb.left(), y - bb.top(), w);
                    Rgbaf out = work[si];
                    if (gate) {
                        const float cov = selection->coverage(x, y);
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
    const Rect src = static_cast<PixelLayer*>(layer)->contentBounds();
    if (src.isEmpty()) return nullptr;  // empty layer: nothing to move
    const Rect dst{src.x + dx, src.y + dy, src.width, src.height};
    // The edit region spans both the vacated source and the destination, so clearing the old
    // pixels and writing the shifted ones is one self-contained in-region transform.
    const Rect region = src.united(dst);
    return bakePixelEditRegion(
        doc, layerId, "Move", region,
        [dx, dy](std::span<Rgbaf> img, int w, int h) {
            const std::vector<Rgbaf> in(img.begin(), img.end());
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int sx = x - dx;  // region-local source of the pixel now at (x,y)
                    const int sy = y - dy;
                    img[idx(x, y, w)] = (sx >= 0 && sx < w && sy >= 0 && sy < h)
                                            ? in[idx(sx, sy, w)]
                                            : Rgbaf{};  // vacated area becomes transparent
                }
            }
        },
        /*selection=*/nullptr);  // the Move tool shifts the whole layer, not a gated region
}

std::unique_ptr<PaintCommand> transformLayerContent(Document& doc, LayerId layerId,
                                                    const Affine2D& srcToDst) {
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

    // The edit spans both the vacated source and the destination: clear the old pixels and write
    // the resampled ones in one reversible in-region transform. bakePixelEditRegion caps the area
    // (kMaxFilterPixels) and the origin/extent, so the destination size is bounded there.
    const Rect region = src.united(dst);
    const Affine2D inv = srcToDst.inverted();
    const int rx = region.x;
    const int ry = region.y;
    return bakePixelEditRegion(
        doc, layerId, "Transform", region,
        [inv, rx, ry](std::span<Rgbaf> img, int w, int h) {
            const std::vector<Rgbaf> orig(img.begin(), img.end());
            // Premultiplied bilinear sample of `orig` at continuous pixel index (fx, fy); fully
            // outside (or non-finite) reads transparent, so the transform never bleeds the
            // arbitrary color of transparent pixels and the int casts below can't go out of range.
            const auto sample = [&](double fx, double fy) -> Rgbaf {
                if (!std::isfinite(fx) || !std::isfinite(fy) || fx < -1.0 || fy < -1.0 ||
                    fx > static_cast<double>(w) || fy > static_cast<double>(h)) {
                    return Rgbaf{};
                }
                const int x0 = static_cast<int>(std::floor(fx));
                const int y0 = static_cast<int>(std::floor(fy));
                const float tx = static_cast<float>(fx - x0);
                const float ty = static_cast<float>(fy - y0);
                const auto at = [&](int xi, int yi) -> Rgbaf {
                    if (xi < 0 || xi >= w || yi < 0 || yi >= h) return Rgbaf{};
                    return premultiply(orig[idx(xi, yi, w)]);
                };
                const auto lerp = [](const Rgbaf& a, const Rgbaf& b, float t) {
                    return Rgbaf{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                                 a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
                };
                const Rgbaf top = lerp(at(x0, y0), at(x0 + 1, y0), tx);
                const Rgbaf bot = lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
                return unpremultiply(lerp(top, bot, ty));
            };
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    // Inverse-map this destination pixel's CENTER to a source document coordinate,
                    // then to a region-local pixel index (center of index i sits at rx+i+0.5).
                    const double ddx = static_cast<double>(rx) + x + 0.5;
                    const double ddy = static_cast<double>(ry) + y + 0.5;
                    const double sxDoc = inv.applyX(ddx, ddy);
                    const double syDoc = inv.applyY(ddx, ddy);
                    img[idx(x, y, w)] = sample(sxDoc - rx - 0.5, syDoc - ry - 0.5);
                }
            }
        },
        /*selection=*/nullptr);  // the Transform tool transforms the whole layer
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

std::unique_ptr<PaintCommand> gradientFill(Document& doc, LayerId layerId, Point start, Point end,
                                           Rgbaf c0, Rgbaf c1, const Selection* selection) {
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
        [start, dx, dy, len2, c0, c1, region](std::span<Rgbaf> img, int w, int h) {
            const std::vector<Rgbaf> orig(img.begin(),
                                          img.end());  // composite the gradient over these
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    // Project the pixel center onto the start->end axis, clamped to [0,1].
                    const double px = region.x + x + 0.5 - start.x;
                    const double py = region.y + y + 0.5 - start.y;
                    double t = (px * dx + py * dy) / len2;
                    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                    const float tf = static_cast<float>(t);
                    const Rgbaf stop{c0.r + (c1.r - c0.r) * tf, c0.g + (c1.g - c0.g) * tf,
                                     c0.b + (c1.b - c0.b) * tf, c0.a + (c1.a - c0.a) * tf};
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
