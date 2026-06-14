#pragma once

#include "pe/core/Layer.hpp"
#include "pe/core/PixelFormat.hpp"
#include "pe/core/TileStore.hpp"

namespace pe {

// A raster layer with sparse, copy-on-write tiled storage. Absent tiles are
// transparent. A layer stores its pixels at one bit depth (U8/U16/F32); the
// matching store is the active one and the others stay empty (a sparse map costs
// nothing). Compositing always reads float (renderInto converts), so depth is an
// internal storage detail. The store is exposed for editing tools/commands.
class PixelLayer final : public Layer {
public:
    explicit PixelLayer(std::string name = "Layer", BitDepth depth = BitDepth::U8);

    [[nodiscard]] BitDepth depth() const noexcept { return depth_; }

    [[nodiscard]] Rect contentBounds() const noexcept override;
    void renderInto(TileCoord coord, std::span<Rgbaf> dst) const override;
    [[nodiscard]] std::unique_ptr<Layer> clone() const override;

    [[nodiscard]] bool hasTileAt(TileCoord c) const noexcept;

    // Editing surface. tiles() is the 8-bit store (the default and what existing
    // construction/tests/paint commands use); tiles16()/tilesF() expose the high-
    // bit-depth stores for U16/F32 layers. Each is the active store only when the
    // layer's depth() matches.
    [[nodiscard]] TileStore& tiles() noexcept { return tiles8_; }
    [[nodiscard]] const TileStore& tiles() const noexcept { return tiles8_; }
    [[nodiscard]] TileStore16& tiles16() noexcept { return tiles16_; }
    [[nodiscard]] const TileStore16& tiles16() const noexcept { return tiles16_; }
    [[nodiscard]] TileStoreF& tilesF() noexcept { return tilesF_; }
    [[nodiscard]] const TileStoreF& tilesF() const noexcept { return tilesF_; }

private:
    BitDepth depth_ = BitDepth::U8;
    TileStore tiles8_;     // active when depth_ == U8
    TileStore16 tiles16_;  // active when depth_ == U16
    TileStoreF tilesF_;    // active when depth_ == F32
};

}  // namespace pe
