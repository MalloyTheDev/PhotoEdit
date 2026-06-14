#include "pe/core/Filter.hpp"

#include "pe/core/Compositor.hpp"  // kMaxCompositeImagePixels
#include "pe/core/Document.hpp"    // kMaxCanvasDimension
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
    std::vector<Rgbaf> tmp(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    convolveH(src, tmp, w, h, kernel, radius);
    convolveV(tmp, dst, w, h, kernel, radius);
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

    std::vector<Rgbaf> tmp(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    convolveH(src, tmp, w, h, kernel, radius);
    convolveV(tmp, dst, w, h, kernel, radius);
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
        if (std::fabs(detail) < threshold) return s;
        return clamp01(s + amount * detail);
    };
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i) {
        const Rgbaf& s = src[i];
        const Rgbaf& b = blurred[i];
        dst[i] = Rgbaf{sharpen(s.r, b.r), sharpen(s.g, b.g), sharpen(s.b, b.b), s.a};
    }
}

std::unique_ptr<PaintCommand> applyFilter(Document& doc, LayerId layerId, const Filter& filter,
                                          const Selection* selection) {
    Layer* layer = doc.findLayer(layerId);
    if (layer == nullptr || layer->kind() != LayerKind::Pixel) return nullptr;
    auto* pl = static_cast<PixelLayer*>(layer);

    const Rect bb = pl->contentBounds();
    if (bb.isEmpty()) return nullptr;
    if (bb.width > kMaxCanvasDimension || bb.height > kMaxCanvasDimension) return nullptr;
    const int64_t area = static_cast<int64_t>(bb.width) * static_cast<int64_t>(bb.height);
    if (area > kMaxCompositeImagePixels) return nullptr;

    const int w = bb.width;
    const int h = bb.height;
    std::vector<Rgbaf> src(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<Rgbaf> dst(src.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            src[idx(x, y, w)] = toFloat(pl->tiles().pixel(bb.left() + x, bb.top() + y));
        }
    }
    filter.apply(src, dst, w, h);

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
                    Rgbaf out = dst[si];
                    if (gate) {
                        const float cov = selection->coverage(x, y);
                        const Rgbaf& orig = src[si];
                        out.r = orig.r + (out.r - orig.r) * cov;
                        out.g = orig.g + (out.g - orig.g) * cov;
                        out.b = orig.b + (out.b - orig.b) * cov;
                        out.a = orig.a + (out.a - orig.a) * cov;
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
    return std::make_unique<PaintCommand>(layerId, dirty, std::move(deltas), filter.displayName());
}

}  // namespace pe
