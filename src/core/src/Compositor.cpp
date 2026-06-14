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

    for (const auto& layerPtr : stack) {
        const Layer* layer = layerPtr.get();
        if (layer == nullptr) continue;
        if (!layer->visible() || layer->opacity() <= 0.0f) continue;

        // Adjustment layers transform the accumulated backdrop instead of
        // contributing pixels (the "twist" in the compositor loop). They cover the
        // whole backdrop, so they are never culled; the mask scopes where they apply.
        if (layer->isAdjustment()) {
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

        // Cull: a layer with no content, or content that cannot touch this tile,
        // contributes nothing.
        const Rect cb = layer->contentBounds();
        if (cb.isEmpty() || !cb.intersects(tile)) continue;

        if (layer->kind() == LayerKind::Group) {
            const auto* group = static_cast<const GroupLayer*>(layer);
            std::fill(src.begin(), src.end(), Rgbaf{});
            compositeStack(group->children(), coord, srcSpan, depth + 1);
        } else {
            layer->renderInto(coord, srcSpan);
        }

        // Layer mask: multiply the layer's coverage (alpha) by the mask before
        // blending, so a gray mask makes the layer partly transparent (color
        // unchanged), orthogonal to blend mode and opacity. Skipped entirely when
        // there is no enabled mask or the mask is a trivial no-op.
        const Mask* mask = layer->mask();
        if (mask != nullptr && mask->enabled()) {
            const bool trivial =
                !mask->inverted() && mask->density() >= 1.0f && mask->buffer().empty();
            if (!trivial) {
                const int baseX = coord.col * kTileSize;
                const int baseY = coord.row * kTileSize;
                std::size_t i = 0;
                for (int ly = 0; ly < kTileSize; ++ly) {
                    for (int lx = 0; lx < kTileSize; ++lx, ++i) {
                        src[i].a *= mask->evaluate(baseX + lx, baseY + ly);
                    }
                }
            }
        }

        const BlendMode mode = layer->blendMode();
        // With no layer effects (M1), fill opacity scales content the same way
        // opacity does, so the effective coverage is their product. When effects
        // arrive they will scale by opacity only; fill opacity stays content-only.
        const float op = layer->opacity() * layer->fillOpacity();
        for (std::size_t i = 0; i < src.size(); ++i) {
            acc[i] = compositeOver(mode, acc[i], src[i], op);
        }
    }
}

PixelBuffer compositeToImage(std::span<const std::unique_ptr<Layer>> stack, Rect canvas) {
    if (canvas.isEmpty()) return PixelBuffer{};

    // Cap the eager allocation. Larger documents are rendered tile-by-tile via the
    // viewport (M2), not flattened whole. Area is computed in int64 (no overflow).
    const int64_t area = static_cast<int64_t>(canvas.width) * static_cast<int64_t>(canvas.height);
    if (area > kMaxCompositeImagePixels) return PixelBuffer{};

    PixelBuffer out(canvas.width, canvas.height, Rgba8{});
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
                    out.set(x - canvas.left(), y - canvas.top(), toRgba8(c));
                }
            }
        }
    }
    return out;
}

}  // namespace pe
