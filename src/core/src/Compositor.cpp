#include "pe/core/Compositor.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"

#include <algorithm>
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
    std::vector<float> baseClipAlpha(static_cast<std::size_t>(kTilePixels), 0.0f);
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
            const bool hasMask = mask != nullptr && mask->enabled();
            const BlendMode mode = layer->blendMode();
            const float op = layer->opacity() * layer->fillOpacity();
            const int baseX = coord.col * kTileSize;
            const int baseY = coord.row * kTileSize;
            for (std::size_t i = 0; i < static_cast<std::size_t>(kTilePixels); ++i) {
                float t = op;
                if (hasMask) {
                    const int lx = static_cast<int>(i % static_cast<std::size_t>(kTileSize));
                    const int ly = static_cast<int>(i / static_cast<std::size_t>(kTileSize));
                    t *= mask->evaluate(baseX + lx, baseY + ly);
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
            if (mask != nullptr && mask->enabled()) {
                const bool trivial =
                    !mask->inverted() && mask->density() >= 1.0f && mask->buffer().empty();
                if (!trivial) {
                    const int bx = coord.col * kTileSize;
                    const int by = coord.row * kTileSize;
                    std::size_t i = 0;
                    for (int ly = 0; ly < kTileSize; ++ly) {
                        for (int lx = 0; lx < kTileSize; ++lx, ++i) {
                            src[i].a *= mask->evaluate(bx + lx, by + ly);
                        }
                    }
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
            std::fill(baseClipAlpha.begin(), baseClipAlpha.end(), 0.0f);
            baseValid = true;
            continue;
        }
        renderSrc(layer);
        for (std::size_t i = 0; i < src.size(); ++i) baseClipAlpha[i] = src[i].a;
        baseValid = true;
        blendSrcInto(layer);
    }
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
