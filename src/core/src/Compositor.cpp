#include "pe/core/Compositor.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/GroupLayer.hpp"

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

    for (const auto& layerPtr : stack) {
        const Layer* layer = layerPtr.get();
        if (layer == nullptr) continue;
        if (!layer->visible() || layer->opacity() <= 0.0f) continue;

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

        const BlendMode mode = layer->blendMode();
        const float op = layer->opacity();
        for (std::size_t i = 0; i < src.size(); ++i) {
            acc[i] = compositeOver(mode, acc[i], src[i], op);
        }
    }
}

PixelBuffer compositeToImage(std::span<const std::unique_ptr<Layer>> stack, Rect canvas) {
    if (canvas.isEmpty()) return PixelBuffer{};

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
