#include "pe/core/PixelLayer.hpp"

#include <algorithm>
#include <cstddef>

namespace pe {

PixelLayer::PixelLayer(std::string name, BitDepth depth)
    : Layer(LayerKind::Pixel, std::move(name)), depth_(depth) {}

Rect PixelLayer::contentBounds() const noexcept {
    switch (depth_) {
        case BitDepth::U16:
            return tiles16_.contentBounds();
        case BitDepth::F32:
            return tilesF_.contentBounds();
        case BitDepth::U8:
        default:
            return tiles8_.contentBounds();
    }
}

bool PixelLayer::hasTileAt(TileCoord c) const noexcept {
    switch (depth_) {
        case BitDepth::U16:
            return tiles16_.hasTileAt(c);
        case BitDepth::F32:
            return tilesF_.hasTileAt(c);
        case BitDepth::U8:
        default:
            return tiles8_.hasTileAt(c);
    }
}

void PixelLayer::renderInto(TileCoord coord, std::span<Rgbaf> dst) const {
    // Contract: dst covers exactly one tile (kTilePixels), tile-local row-major.
    // Read the active store at its native depth and convert to float; an absent
    // tile is transparent. (toFloat is the identity for the float store.)
    // Clamp to the fixed tile size: the per-tile arrays are exactly kTilePixels, so a
    // larger dst (contract violation) must never index past them.
    const std::size_t n = std::min(dst.size(), static_cast<std::size_t>(kTilePixels));
    switch (depth_) {
        case BitDepth::U16: {
            const TileData16* t = tiles16_.find(coord);
            if (t == nullptr) {
                for (auto& p : dst) p = Rgbaf{};
                return;
            }
            for (std::size_t i = 0; i < n; ++i) dst[i] = toFloat(t->px[i]);
            return;
        }
        case BitDepth::F32: {
            const TileDataF* t = tilesF_.find(coord);
            if (t == nullptr) {
                for (auto& p : dst) p = Rgbaf{};
                return;
            }
            for (std::size_t i = 0; i < n; ++i) dst[i] = t->px[i];
            return;
        }
        case BitDepth::U8:
        default: {
            const TileData* t = tiles8_.find(coord);
            if (t == nullptr) {
                for (auto& p : dst) p = Rgbaf{};
                return;
            }
            for (std::size_t i = 0; i < n; ++i) dst[i] = toFloat(t->px[i]);
            return;
        }
    }
}

std::unique_ptr<Layer> PixelLayer::clone() const {
    auto copy = std::make_unique<PixelLayer>(name(), depth_);
    copyPropsTo(*copy);
    // Shallow-clone the stores: tiles are shared copy-on-write, so this is cheap and
    // a later edit to either layer forks only the touched tiles. Inactive stores are
    // empty maps, so cloning all three is effectively free.
    copy->tiles8_ = tiles8_.shallowClone();
    copy->tiles16_ = tiles16_.shallowClone();
    copy->tilesF_ = tilesF_.shallowClone();
    return copy;
}

}  // namespace pe
