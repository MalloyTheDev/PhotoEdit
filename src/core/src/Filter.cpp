#include "pe/core/Filter.hpp"

#include "pe/core/Document.hpp"  // kMaxCanvasDimension
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"

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
    if (sigma <= 0.0f) {
        copyImage(src, dst);
        return;
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
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

std::unique_ptr<PaintCommand> bakePixelEdit(
    Document& doc, LayerId layerId, std::string name,
    const std::function<void(std::span<Rgbaf>, int, int)>& transform, const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);

    const Rect bb = pl->contentBounds();
    if (bb.isEmpty()) return nullptr;
    if (bb.width > kMaxCanvasDimension || bb.height > kMaxCanvasDimension) return nullptr;
    const int64_t area = static_cast<int64_t>(bb.width) * static_cast<int64_t>(bb.height);
    if (area > kMaxFilterPixels) return nullptr;

    const int w = bb.width;
    const int h = bb.height;
    std::vector<Rgbaf> orig(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            orig[idx(x, y, w)] = toFloat(pl->tiles().pixel(bb.left() + x, bb.top() + y));
        }
    }
    std::vector<Rgbaf> work = orig;  // the transform mutates this in place
    transform(std::span<Rgbaf>(work), w, h);

    const bool gate = selection != nullptr && selection->active();
    std::vector<PaintCommand::Delta> deltas;
    Rect dirty{};
    const TileSpan span = tilesForRect(bb);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(bb);
            if (vis.isEmpty()) continue;
            std::shared_ptr<TileData> before = pl->tiles().sharedTile(coord);
            auto after = std::make_shared<TileData>();
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
                    const Rgba8 np = toRgba8(out);
                    const std::size_t li = static_cast<std::size_t>(y - tb.top()) * kTileSize +
                                           static_cast<std::size_t>(x - tb.left());
                    if (!(np == after->px[li])) {
                        after->px[li] = np;
                        changed = true;
                    }
                }
            }
            if (changed) {
                deltas.push_back(PaintCommand::Delta{coord, std::move(before), std::move(after)});
                dirty = dirty.united(vis);
            }
        }
    }
    if (deltas.empty()) return nullptr;
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), std::move(name));
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

}  // namespace pe
