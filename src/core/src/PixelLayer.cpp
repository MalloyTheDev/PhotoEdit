#include "pe/core/PixelLayer.hpp"

#include "pe/core/Color.hpp"
#include "pe/core/Tile.hpp"

#include <algorithm>
#include <cstddef>

namespace pe {

PixelLayer::PixelLayer(std::string name, BitDepth depth)
    : Layer(LayerKind::Pixel, std::move(name)), depth_(depth) {}

namespace {
// Copy every materialised tile of `src` into `dst`, converting each pixel through float. Whole
// tiles at a time (a converted tile is dense), so it is O(content), matching the stores' sparsity.
template <class Src, class Dst>
void convertStore(const TileStoreT<Src>& src, TileStoreT<Dst>& dst) {
    const Rect cb = src.contentBounds();
    if (cb.isEmpty()) return;
    const TileSpan span = tilesForRect(cb);
    for (int trow = span.rowBegin; trow < span.rowEnd; ++trow) {
        for (int tcol = span.colBegin; tcol < span.colEnd; ++tcol) {
            const TileCoord c{tcol, trow};
            const TileDataT<Src>* t = src.find(c);
            if (t == nullptr) continue;
            const Rect tb = tileBounds(c);
            for (int ly = 0; ly < kTileSize; ++ly) {
                for (int lx = 0; lx < kTileSize; ++lx) {
                    dst.setPixel(tb.left() + lx, tb.top() + ly,
                                 fromFloat<Dst>(toFloat(t->at(lx, ly))));
                }
            }
        }
    }
}
}  // namespace

void PixelLayer::convertDepth(BitDepth target) {
    if (target == depth_) return;
    switch (depth_) {
        case BitDepth::U8:
            if (target == BitDepth::U16) {
                convertStore<Rgba8, Rgba16>(tiles8_, tiles16_);
            } else {
                convertStore<Rgba8, Rgbaf>(tiles8_, tilesF_);
            }
            tiles8_ = TileStore{};
            break;
        case BitDepth::U16:
            if (target == BitDepth::U8) {
                convertStore<Rgba16, Rgba8>(tiles16_, tiles8_);
            } else {
                convertStore<Rgba16, Rgbaf>(tiles16_, tilesF_);
            }
            tiles16_ = TileStore16{};
            break;
        case BitDepth::F32:
            if (target == BitDepth::U8) {
                convertStore<Rgbaf, Rgba8>(tilesF_, tiles8_);
            } else {
                convertStore<Rgbaf, Rgba16>(tilesF_, tiles16_);
            }
            tilesF_ = TileStoreF{};
            break;
    }
    depth_ = target;
}

void PixelLayer::restorePixelState(const PixelLayer& other) {
    depth_ = other.depth_;
    tiles8_ = other.tiles8_;
    tiles16_ = other.tiles16_;
    tilesF_ = other.tilesF_;
}

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
