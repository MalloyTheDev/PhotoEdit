#pragma once

#include "pe/core/Layer.hpp"
#include "pe/core/TileStore.hpp"

namespace pe {

// A raster layer with sparse, copy-on-write tiled storage. Absent tiles are
// transparent. The store is exposed for editing tools/commands (M3+); in M1 it
// is populated directly for construction and tests.
class PixelLayer final : public Layer {
public:
    explicit PixelLayer(std::string name = "Layer");

    [[nodiscard]] Rect contentBounds() const noexcept override;
    void renderInto(TileCoord coord, std::span<Rgbaf> dst) const override;
    [[nodiscard]] std::unique_ptr<Layer> clone() const override;

    [[nodiscard]] bool hasTileAt(TileCoord c) const noexcept { return tiles_.hasTileAt(c); }

    // Editing surface (used by construction, tests, and painting commands later).
    [[nodiscard]] TileStore& tiles() noexcept { return tiles_; }
    [[nodiscard]] const TileStore& tiles() const noexcept { return tiles_; }

private:
    TileStore tiles_;
};

}  // namespace pe
