#include "pe/core/PixelLayer.hpp"

#include <cstddef>

namespace pe {

PixelLayer::PixelLayer(std::string name) : Layer(LayerKind::Pixel, std::move(name)) {}

Rect PixelLayer::contentBounds() const noexcept {
    return tiles_.contentBounds();
}

void PixelLayer::renderInto(TileCoord coord, std::span<Rgbaf> dst) const {
    // Contract: dst covers exactly one tile (kTilePixels), tile-local row-major.
    const TileData* t = tiles_.find(coord);
    if (t == nullptr) {
        for (auto& p : dst) p = Rgbaf{};  // transparent
        return;
    }
    const std::size_t n = dst.size();
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = toFloat(t->px[i]);
    }
}

std::unique_ptr<Layer> PixelLayer::clone() const {
    auto copy = std::make_unique<PixelLayer>(name());
    copyPropsTo(*copy);
    // Shallow-clone the tile store: tiles are shared copy-on-write, so this is
    // cheap and a later edit to either layer forks only the touched tiles.
    copy->tiles_ = tiles_.shallowClone();
    return copy;
}

}  // namespace pe
