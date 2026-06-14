#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>

namespace pe {

// The pixel payload of one 256x256 tile. Reference-counted and shared; mutation
// goes through TileStore which forks a private copy when a tile is shared
// (copy-on-write). Default-constructed tiles are fully transparent.
struct TileData {
    std::array<Rgba8, kTilePixels> px{};  // row-major, value-initialized to {0,0,0,0}

    [[nodiscard]] Rgba8 at(int lx, int ly) const noexcept {
        return px[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)];
    }
    void set(int lx, int ly, Rgba8 c) noexcept {
        px[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] = c;
    }
};

// Sparse, copy-on-write tiled pixel storage for a layer. Absent tiles read as
// transparent, so an empty or mostly-empty layer costs almost nothing. See
// docs/systems/03-layer-system.md and ADR-0003.
class TileStore {
public:
    // Pixel read at any document coordinate (negative allowed). Out-of-store
    // (absent tile) reads as transparent.
    [[nodiscard]] Rgba8 pixel(int x, int y) const noexcept;

    // Pixel write at any document coordinate. Forks the target tile if shared.
    void setPixel(int x, int y, Rgba8 c);

    // Fill a document-space rect (clipped to nothing else; may create tiles).
    void fillRect(Rect r, Rgba8 c);

    // The tile at this coord, or nullptr if absent (== transparent).
    [[nodiscard]] const TileData* find(TileCoord c) const noexcept;

    // Get a mutable tile, creating it if absent and forking it if shared (COW).
    [[nodiscard]] TileData& editable(TileCoord c);

    [[nodiscard]] bool hasTileAt(TileCoord c) const noexcept;
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }

    // Smallest document-space rect covering all occupied tiles (empty if none).
    [[nodiscard]] Rect contentBounds() const noexcept;

    // A shallow copy that SHARES tiles (copy-on-write). This is the snapshot /
    // duplicate-layer mechanism: unchanged tiles stay shared until written.
    [[nodiscard]] TileStore shallowClone() const { return TileStore(*this); }

    // For testing/diagnostics: how many tiles this store uniquely owns
    // (use_count == 1). Used to assert COW sharing semantics.
    [[nodiscard]] std::size_t uniquelyOwnedTileCount() const noexcept;

    template <class F>
    void forEachTile(F&& f) const {
        for (const auto& [key, data] : tiles_) {
            f(TileCoord{key.first, key.second}, *data);
        }
    }

private:
    using Key = std::pair<int, int>;  // {col, row}, ordered for std::map
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    std::map<Key, std::shared_ptr<TileData>> tiles_;
};

}  // namespace pe
