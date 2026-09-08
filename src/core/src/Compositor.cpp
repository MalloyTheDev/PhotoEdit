#include "pe/core/Compositor.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace pe {

void compositeStack(std::span<const std::unique_ptr<Layer>> stack, TileCoord coord,
                    std::span<Rgbaf> acc, int depth) {
    if (depth >= kMaxCompositeDepth) return;  // too deep: contribute nothing
    if (acc.size() < static_cast<std::size_t>(kTilePixels)) return;

    const Rect tile = tileBounds(coord);

    // One reusable scratch buffer for this stack level; refilled per layer.
    std::vector<Rgbaf> src(static_cast<std::size_t>(kTilePixels));
    const std::span<Rgbaf> srcSpan(src);
    std::vector<Rgbaf> adjusted;  // lazily sized when an adjustment layer is hit

    // Coverage of the most recent non-clipped (base) layer, so clipped layers above
    // can be confined to it. baseValid is false until a base layer is seen.
    //
    // Only a CLIPPED layer ever reads this, and clipping never crosses a group boundary (the
    // recursion below hands each child stack its own), so one pass over this stack settles
    // whether it is needed at all. Most documents have no clipped layer, and this used to be
    // allocated and zero-filled unconditionally, per tile, per stack level, then written once
    // per pixel for every visible base layer and never read. A full pass over a document at
    // the project's target size is about 3.6 GB of allocate-and-write for nothing.
    const bool anyClipped = std::any_of(stack.begin(), stack.end(),
                                        [](const auto& l) { return l != nullptr && l->clipped(); });
    std::vector<float> baseClipAlpha;
    if (anyClipped) baseClipAlpha.assign(static_cast<std::size_t>(kTilePixels), 0.0f);
    bool baseValid = false;

    for (const auto& layerPtr : stack) {
        const Layer* layer = layerPtr.get();
        if (layer == nullptr) continue;
        const bool hidden = !layer->visible() || layer->opacity() <= 0.0f;

        // Adjustment layers transform the accumulated backdrop instead of
        // contributing pixels (the "twist" in the compositor loop). They cover the
        // whole backdrop, so they are never culled; the mask scopes where they apply.
        if (layer->isAdjustment()) {
            if (hidden) continue;
            if (adjusted.size() < static_cast<std::size_t>(kTilePixels)) {
                adjusted.assign(static_cast<std::size_t>(kTilePixels), Rgbaf{});
            }
            std::copy(acc.begin(), acc.begin() + kTilePixels, adjusted.begin());
            layer->applyTo(std::span<Rgbaf>(adjusted.data(), kTilePixels), coord);

            const Mask* mask = layer->mask();
            // A mask that reveals everything at full strength multiplies by 1, so skip it
            // outright rather than paying a lookup per pixel for no effect. The
            // layer-mask path below has always had this guard; this one had not, so an
            // adjustment carrying an empty mask (what maskFromSelection returns for an
            // inactive selection) paid full price.
            const bool hasMask = mask != nullptr && mask->enabled() && !mask->isFullyRevealing();
            // The whole tile in one lookup instead of 65,536: the loop never leaves the
            // tile at `coord`, so the mask tile is the same for every iteration. Absent
            // reads as kOpaque, and that byte still goes through evaluateValue so invert
            // and density apply to it exactly as before.
            const MaskBuffer::GrayTile* maskTile =
                hasMask ? mask->buffer().findTile(coord) : nullptr;
            const BlendMode mode = layer->blendMode();
            const float op = layer->opacity() * layer->fillOpacity();
            for (std::size_t i = 0; i < static_cast<std::size_t>(kTilePixels); ++i) {
                float t = op;
                if (hasMask) {
                    t *= mask->evaluateValue(maskTile != nullptr ? (*maskTile)[i]
                                                                 : MaskBuffer::kOpaque);
                }
                if (t <= 0.0f) continue;
                Rgbaf& a = acc[i];
                const Rgbaf& d = adjusted[i];
                // Blend the adjusted color back over the backdrop via the layer's
                // blend mode, mixed in by t (opacity x mask). This is intentionally
                // an ALPHA-PRESERVING color lerp (not compositeOver): an adjustment
                // modifies existing pixels' color, it never adds coverage. Exact for
                // Normal and any opaque backdrop; per-mode parity on partially
                // transparent backdrops is a later refinement.
                a.r += t * (blendChannel(mode, a.r, d.r) - a.r);
                a.g += t * (blendChannel(mode, a.g, d.g) - a.g);
                a.b += t * (blendChannel(mode, a.b, d.b) - a.b);
            }
            continue;
        }

        // --- non-adjustment layers (pixel/fill/group), with clipping support ---

        // Render this layer's straight-alpha contribution into `src`, then apply its
        // own mask. Used by both base and clipped layers.
        const auto renderSrc = [&](const Layer* l) {
            if (l->kind() == LayerKind::Group) {
                const auto* group = static_cast<const GroupLayer*>(l);
                std::fill(src.begin(), src.end(), Rgbaf{});
                compositeStack(group->children(), coord, srcSpan, depth + 1);
            } else {
                l->renderInto(coord, srcSpan);
            }
            const Mask* mask = l->mask();
            if (mask != nullptr && mask->enabled() && !mask->isFullyRevealing()) {
                // One lookup for the tile, then a flat index. `i` counts ly * kTileSize +
                // lx, which is exactly the index MaskBuffer::value computes internally,
                // so this reads the same byte for every pixel it used to.
                const MaskBuffer::GrayTile* maskTile = mask->buffer().findTile(coord);
                for (std::size_t i = 0; i < static_cast<std::size_t>(kTilePixels); ++i) {
                    src[i].a *= mask->evaluateValue(maskTile != nullptr ? (*maskTile)[i]
                                                                        : MaskBuffer::kOpaque);
                }
            }
        };

        const auto blendSrcInto = [&](const Layer* l) {
            const BlendMode mode = l->blendMode();
            const float op = l->opacity() * l->fillOpacity();
            for (std::size_t i = 0; i < src.size(); ++i)
                acc[i] = compositeOver(mode, acc[i], src[i], op);
        };

        const Rect cb = layer->contentBounds();
        const bool touches = !cb.isEmpty() && cb.intersects(tile);

        if (layer->clipped()) {
            // Confine a clipped layer to the base layer's coverage. A hidden or
            // off-tile clipped layer contributes nothing (and does not affect the
            // base for other clipped layers).
            if (hidden || !touches) continue;
            renderSrc(layer);
            if (baseValid) {
                for (std::size_t i = 0; i < src.size(); ++i) src[i].a *= baseClipAlpha[i];
            }
            blendSrcInto(layer);
            continue;  // clipped layers do not become a base
        }

        // A non-clipped layer is the base for any clipped layers above it. A base
        // that is HIDDEN or has no content on this tile records ZERO coverage, so a
        // clipped run above it is hidden here (per the layer-system spec).
        if (hidden || !touches) {
            if (anyClipped) std::fill(baseClipAlpha.begin(), baseClipAlpha.end(), 0.0f);
            baseValid = true;
            continue;
        }
        renderSrc(layer);
        if (anyClipped) {
            for (std::size_t i = 0; i < src.size(); ++i) baseClipAlpha[i] = src[i].a;
        }
        baseValid = true;
        blendSrcInto(layer);
    }
}

namespace {
std::atomic<std::uint64_t> g_scaledCompositeTiles{0};
}  // namespace

std::uint64_t scaledCompositeTileCount() noexcept {
    return g_scaledCompositeTiles.load(std::memory_order_relaxed);
}

PixelBuffer compositeToImageScaled(std::span<const std::unique_ptr<Layer>> stack, Rect region,
                                   int divisor, std::int64_t maxSourceTiles) {
    if (region.isEmpty() || divisor < 1) return PixelBuffer{};

    // Same coordinate guard compositeToBuffer applies: an enormous offset would overflow the
    // int tile and rect arithmetic even though the OUTPUT here is small.
    constexpr int kCoordBound = 1 << 26;
    if (region.x < -kCoordBound || region.y < -kCoordBound ||
        static_cast<int64_t>(region.x) + region.width > kCoordBound ||
        static_cast<int64_t>(region.y) + region.height > kCoordBound) {
        return PixelBuffer{};
    }

    const TileSpan span = tilesForRect(region);
    const int64_t srcTiles = static_cast<int64_t>(span.rowEnd - span.rowBegin) *
                             static_cast<int64_t>(span.colEnd - span.colBegin);
    if (srcTiles > maxSourceTiles) return PixelBuffer{};

    const int ow = (region.width + divisor - 1) / divisor;
    const int oh = (region.height + divisor - 1) / divisor;
    const std::size_t outCount = static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh);
    std::vector<double> sumR(outCount, 0.0);
    std::vector<double> sumG(outCount, 0.0);
    std::vector<double> sumB(outCount, 0.0);
    std::vector<double> sumA(outCount, 0.0);

    std::vector<Rgbaf> acc(static_cast<std::size_t>(kTilePixels));
    const std::span<Rgbaf> accSpan(acc);
    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            const Rect tb = tileBounds(coord);
            const Rect vis = tb.intersected(region);
            if (vis.isEmpty()) continue;
            g_scaledCompositeTiles.fetch_add(1, std::memory_order_relaxed);
            std::fill(acc.begin(), acc.end(), Rgbaf{});
            compositeStack(stack, coord, accSpan, 0);
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const int oy = (y - region.top()) / divisor;
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const int ox = (x - region.left()) / divisor;
                    const std::size_t bin =
                        static_cast<std::size_t>(oy) * static_cast<std::size_t>(ow) +
                        static_cast<std::size_t>(ox);
                    const Rgbaf& c = acc[static_cast<std::size_t>(y - tb.top()) * kTileSize +
                                         static_cast<std::size_t>(x - tb.left())];
                    sumR[bin] += static_cast<double>(c.r) * c.a;  // premultiplied
                    sumG[bin] += static_cast<double>(c.g) * c.a;
                    sumB[bin] += static_cast<double>(c.b) * c.a;
                    sumA[bin] += c.a;
                }
            }
        }
    }

    PixelBuffer out(ow, oh, Rgba8{});
    for (int oy = 0; oy < oh; ++oy) {
        // Samples this output row actually covers, clipped at the region's edge, so a partial
        // final row is not divided by a full one.
        const int srcH = std::min((oy + 1) * divisor, region.height) - oy * divisor;
        for (int ox = 0; ox < ow; ++ox) {
            const int srcW = std::min((ox + 1) * divisor, region.width) - ox * divisor;
            const std::size_t bin = static_cast<std::size_t>(oy) * static_cast<std::size_t>(ow) +
                                    static_cast<std::size_t>(ox);
            const double n = static_cast<double>(srcW) * static_cast<double>(srcH);
            Rgbaf px{};
            if (n > 0.0) {
                px.a = static_cast<float>(sumA[bin] / n);
                if (sumA[bin] > 0.0) {  // un-premultiply back to straight alpha
                    px.r = static_cast<float>(sumR[bin] / sumA[bin]);
                    px.g = static_cast<float>(sumG[bin] / sumA[bin]);
                    px.b = static_cast<float>(sumB[bin] / sumA[bin]);
                }
            }
            out.set(ox, oy, toRgba8(px));
        }
    }
    return out;
}

namespace {
// Shared whole-image flatten used by both the 8-bit and float entry points: walk
// the canvas tile-by-tile, composite each in float, and write each visible pixel
// through `convert` into a buffer of matching depth. Both depths therefore run the
// exact same tile logic and cannot drift. `Pixel{}` is the buffer's clear value.
template <class Buffer, class Pixel, class Convert>
Buffer compositeToBuffer(std::span<const std::unique_ptr<Layer>> stack, Rect canvas,
                         Convert convert) {
    if (canvas.isEmpty()) return Buffer{};

    // The area cap below bounds the buffer, but huge coordinate *offsets* would still
    // overflow the int tile/rect math (right()/bottom(), col*kTileSize). The document
    // canvas is always at the origin; this guards the standalone API against a
    // pathological rect. ~67M keeps right()/bottom() well within int.
    constexpr int kCoordBound = 1 << 26;
    if (canvas.x < -kCoordBound || canvas.y < -kCoordBound ||
        static_cast<int64_t>(canvas.x) + canvas.width > kCoordBound ||
        static_cast<int64_t>(canvas.y) + canvas.height > kCoordBound) {
        return Buffer{};
    }

    // Cap the eager allocation. Larger documents are rendered tile-by-tile via the
    // viewport (M2), not flattened whole. Area is computed in int64 (no overflow).
    const int64_t area = static_cast<int64_t>(canvas.width) * static_cast<int64_t>(canvas.height);
    if (area > kMaxCompositeImagePixels) return Buffer{};

    Buffer out(canvas.width, canvas.height, Pixel{});
    const TileSpan span = tilesForRect(canvas);

    std::vector<Rgbaf> acc(static_cast<std::size_t>(kTilePixels));
    const std::span<Rgbaf> accSpan(acc);

    for (int row = span.rowBegin; row < span.rowEnd; ++row) {
        for (int col = span.colBegin; col < span.colEnd; ++col) {
            const TileCoord coord{col, row};
            std::fill(acc.begin(), acc.end(), Rgbaf{});
            compositeStack(stack, coord, accSpan, 0);

            const Rect tile = tileBounds(coord);
            const Rect vis = tile.intersected(canvas);
            for (int y = vis.top(); y < vis.bottom(); ++y) {
                const int ly = y - tile.top();
                for (int x = vis.left(); x < vis.right(); ++x) {
                    const int lx = x - tile.left();
                    const Rgbaf c = acc[static_cast<std::size_t>(ly) * kTileSize +
                                        static_cast<std::size_t>(lx)];
                    out.set(x - canvas.left(), y - canvas.top(), convert(c));
                }
            }
        }
    }
    return out;
}
}  // namespace

PixelBuffer compositeToImage(std::span<const std::unique_ptr<Layer>> stack, Rect canvas) {
    return compositeToBuffer<PixelBuffer, Rgba8>(stack, canvas,
                                                 [](const Rgbaf& c) { return toRgba8(c); });
}

PixelBufferF compositeToImageF(std::span<const std::unique_ptr<Layer>> stack, Rect canvas) {
    // No quantization: the float composite is preserved at full precision.
    return compositeToBuffer<PixelBufferF, Rgbaf>(stack, canvas, [](const Rgbaf& c) { return c; });
}

PixelBuffer16 compositeToImage16(std::span<const std::unique_ptr<Layer>> stack, Rect canvas) {
    // Quantize the float composite to 16-bit (round + clamp + NaN-sink via toRgba16).
    return compositeToBuffer<PixelBuffer16, Rgba16>(stack, canvas,
                                                    [](const Rgbaf& c) { return toRgba16(c); });
}

}  // namespace pe
