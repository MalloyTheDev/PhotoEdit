#include "pe/core/RHI.hpp"

#include "pe/core/Tile.hpp"

#include <algorithm>
#include <cstring>

namespace pe {

void RHISoftware::uploadTile(TileCoord coord, std::span<const Rgba8> pixels) {
    // For skeleton, just size the backing if needed and copy tile
    if (backing_.empty()) {
        // Assume full doc for simple; real would be sparse
        width_ = 1024; height_ = 1024; // placeholder
        backing_.resize(static_cast<size_t>(width_) * height_);
    }
    const Rect tb = tileBounds(coord);
    if (tb.x + kTileSize > width_ || tb.y + kTileSize > height_) return;
    for (int y = 0; y < kTileSize; ++y) {
        for (int x = 0; x < kTileSize; ++x) {
            int idx = (tb.y + y) * width_ + (tb.x + x);
            if (idx < static_cast<int>(backing_.size()))
                backing_[idx] = pixels[y * kTileSize + x];
        }
    }
}

void RHISoftware::presentRegion(Rect region, std::span<Rgba8> out) {
    // Software: copy from backing (or just zero for skeleton)
    std::fill(out.begin(), out.end(), Rgba8{0,0,0,255});
    // In real integration, would composite from cached tiles or delegate to CPU
}

std::unique_ptr<RHIDevice> createSoftwareRHI() {
    return std::make_unique<RHISoftware>();
}

} // namespace pe
