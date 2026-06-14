#include "pe/core/SolidColorLayer.hpp"

#include <cstddef>

namespace pe {

SolidColorLayer::SolidColorLayer(Rgba8 color, Rect bounds, std::string name)
    : Layer(LayerKind::Fill, std::move(name)), color_(color), bounds_(bounds) {}

void SolidColorLayer::renderInto(TileCoord coord, std::span<Rgbaf> dst) const {
    const Rgbaf fill = toFloat(color_);
    const Rect tile = tileBounds(coord);
    const Rect hit = tile.intersected(bounds_);

    // Fast paths: tile entirely outside or entirely inside the fill rect.
    if (hit.isEmpty()) {
        for (auto& p : dst) p = Rgbaf{};
        return;
    }
    if (hit == tile) {
        for (auto& p : dst) p = fill;
        return;
    }

    // Mixed tile: fill only the covered pixels, transparent elsewhere.
    const int baseX = coord.col * kTileSize;
    const int baseY = coord.row * kTileSize;
    for (int ly = 0; ly < kTileSize; ++ly) {
        const int docY = baseY + ly;
        const bool yIn = docY >= bounds_.top() && docY < bounds_.bottom();
        for (int lx = 0; lx < kTileSize; ++lx) {
            const int docX = baseX + lx;
            const bool in = yIn && docX >= bounds_.left() && docX < bounds_.right();
            dst[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] =
                in ? fill : Rgbaf{};
        }
    }
}

std::unique_ptr<Layer> SolidColorLayer::clone() const {
    auto copy = std::make_unique<SolidColorLayer>(color_, bounds_, name());
    copyPropsTo(*copy);
    return copy;
}

}  // namespace pe
