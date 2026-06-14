#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <utility>

namespace pe {

// Local pixel offset within a tile for a document coordinate. Correct for negative
// coordinates: the result is always in [0, kTileSize).
inline int tileLocalOffset(int coord) noexcept {
    int m = coord % kTileSize;
    if (m < 0) m += kTileSize;
    return m;
}

// The pixel payload of one 256x256 tile, parameterized on the stored pixel type
// (8-bit Rgba8, 16-bit Rgba16, or 32-bit-float Rgbaf). Reference-counted and
// shared; mutation goes through TileStoreT which forks a private copy when a tile
// is shared (copy-on-write). Default-constructed tiles are fully transparent
// (Pixel{} has zero alpha for every supported pixel type).
template <class Pixel>
struct TileDataT {
    std::array<Pixel, kTilePixels> px{};  // row-major, value-initialized transparent

    [[nodiscard]] Pixel at(int lx, int ly) const noexcept {
        return px[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)];
    }
    void set(int lx, int ly, Pixel c) noexcept {
        px[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] = c;
    }
};

// Sparse, copy-on-write tiled pixel storage for a layer, parameterized on the pixel
// type. Absent tiles read as transparent, so an empty or mostly-empty layer costs
// almost nothing. See docs/systems/03-layer-system.md and ADR-0003.
template <class Pixel>
class TileStoreT {
public:
    using Tile = TileDataT<Pixel>;

    // Pixel read at any document coordinate (negative allowed). Out-of-store
    // (absent tile) reads as transparent.
    [[nodiscard]] Pixel pixel(int x, int y) const noexcept {
        const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
        const Tile* t = find(c);
        if (t == nullptr) return Pixel{};  // transparent
        return t->at(tileLocalOffset(x), tileLocalOffset(y));
    }

    // Pixel write at any document coordinate. Forks the target tile if shared.
    void setPixel(int x, int y, Pixel c) {
        const TileCoord coord{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
        Tile& t = editable(coord);
        t.set(tileLocalOffset(x), tileLocalOffset(y), c);
    }

    // Fill a document-space rect (clipped to nothing else; may create tiles).
    void fillRect(Rect r, Pixel c) {
        if (r.isEmpty()) return;
        for (int y = r.top(); y < r.bottom(); ++y) {
            for (int x = r.left(); x < r.right(); ++x) {
                setPixel(x, y, c);
            }
        }
    }

    // The tile at this coord, or nullptr if absent (== transparent).
    [[nodiscard]] const Tile* find(TileCoord c) const noexcept {
        auto it = tiles_.find(keyOf(c));
        return it == tiles_.end() ? nullptr : it->second.get();
    }

    // Get a mutable tile, creating it if absent and forking it if shared (COW).
    [[nodiscard]] Tile& editable(TileCoord c) {
        const Key key = keyOf(c);
        auto it = tiles_.find(key);
        if (it == tiles_.end()) {
            it = tiles_.emplace(key, std::make_shared<Tile>()).first;
            return *it->second;
        }
        // Copy-on-write: if any other owner (an undo snapshot or a duplicated layer)
        // shares this tile, fork a private copy before mutating.
        //
        // SOUNDNESS NOTE (single-threaded today): this use_count() check is a safe
        // fork trigger only because mutation and all readers run on one thread. When
        // multithreaded compositing lands (docs/systems/22-performance.md), workers
        // must be handed shared_ptr<const TileData> snapshots and a concurrent
        // composite pass must force-fork on write (use_count() is racy: a worker can
        // drop its reference between the check and the mutation). Do NOT rely on this
        // check for thread safety without that change.
        if (it->second.use_count() > 1) {
            it->second = std::make_shared<Tile>(*it->second);
        }
        return *it->second;
    }

    [[nodiscard]] bool hasTileAt(TileCoord c) const noexcept {
        return tiles_.find(keyOf(c)) != tiles_.end();
    }

    // Tile-delta support (for PaintCommand undo). sharedTile() returns the stored
    // shared tile pointer (or nullptr if absent) WITHOUT forking; holding it keeps
    // the prior bytes alive (a later editable() write forks, copy-on-write). The
    // returned snapshot must be treated as immutable. setTile() replaces (or, with
    // a null pointer, removes) the tile — used to restore a snapshot on undo.
    [[nodiscard]] std::shared_ptr<Tile> sharedTile(TileCoord c) const {
        auto it = tiles_.find(keyOf(c));
        return it == tiles_.end() ? nullptr : it->second;
    }
    void setTile(TileCoord c, std::shared_ptr<Tile> data) {
        const Key key = keyOf(c);
        if (data == nullptr) {
            tiles_.erase(key);
        } else {
            tiles_[key] = std::move(data);
        }
    }
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }

    // Smallest document-space rect covering all occupied tiles (empty if none).
    [[nodiscard]] Rect contentBounds() const noexcept {
        Rect bounds{};
        for (const auto& [key, data] : tiles_) {
            (void)data;
            bounds = bounds.united(tileBounds(TileCoord{key.first, key.second}));
        }
        return bounds;
    }

    // A shallow copy that SHARES tiles (copy-on-write). This is the snapshot /
    // duplicate-layer mechanism: unchanged tiles stay shared until written.
    [[nodiscard]] TileStoreT shallowClone() const { return TileStoreT(*this); }

    // For testing/diagnostics: how many tiles this store uniquely owns
    // (use_count == 1). Used to assert COW sharing semantics.
    [[nodiscard]] std::size_t uniquelyOwnedTileCount() const noexcept {
        std::size_t n = 0;
        for (const auto& [key, data] : tiles_) {
            (void)key;
            if (data.use_count() == 1) ++n;
        }
        return n;
    }

    template <class F>
    void forEachTile(F&& f) const {
        for (const auto& [key, data] : tiles_) {
            f(TileCoord{key.first, key.second}, *data);
        }
    }

private:
    using Key = std::pair<int, int>;  // {col, row}, ordered for std::map
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    std::map<Key, std::shared_ptr<Tile>> tiles_;
};

// 8-bit storage is the default and what every existing caller uses; the 16-bit and
// float variants enable high-bit-depth documents (docs/systems/15-color-management).
using TileData = TileDataT<Rgba8>;
using TileStore = TileStoreT<Rgba8>;
using TileData16 = TileDataT<Rgba16>;
using TileStore16 = TileStoreT<Rgba16>;
using TileDataF = TileDataT<Rgbaf>;
using TileStoreF = TileStoreT<Rgbaf>;

}  // namespace pe
