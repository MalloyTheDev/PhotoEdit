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

    TileStoreT() = default;
    ~TileStoreT() = default;
    TileStoreT(TileStoreT&&) noexcept = default;
    TileStoreT& operator=(TileStoreT&&) noexcept = default;

    // Copying SHARES every tile buffer, so both stores must fork before writing. Written
    // out rather than defaulted precisely to establish that: a defaulted copy would leave
    // both sides believing they owned their buffers privately, and the first write to
    // either would land in the other's pixels.
    TileStoreT(const TileStoreT& other)
        : tiles_(other.tiles_), bounds_(other.bounds_), boundsDirty_(other.boundsDirty_) {
        other.markAllShared();
        markAllShared();
    }
    TileStoreT& operator=(const TileStoreT& other) {
        if (this == &other) return *this;
        tiles_ = other.tiles_;
        bounds_ = other.bounds_;
        boundsDirty_ = other.boundsDirty_;
        other.markAllShared();
        markAllShared();
        return *this;
    }

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
        return it == tiles_.end() ? nullptr : it->second.data.get();
    }

    [[nodiscard]] bool hasTileAt(TileCoord c) const noexcept {
        return tiles_.find(keyOf(c)) != tiles_.end();
    }

    // Tile-delta support (for PaintCommand undo). sharedTile() returns the stored
    // shared tile pointer (or nullptr if absent) WITHOUT forking; holding it keeps
    // the prior bytes alive, because handing it out marks the tile shared and the
    // next in-place write forks. The returned tile must be treated as immutable.
    // setTile() replaces (or, with a null pointer, removes) the tile, which is how an
    // undo restores a snapshot.
    [[nodiscard]] std::shared_ptr<Tile> sharedTile(TileCoord c) const {
        auto it = tiles_.find(keyOf(c));
        if (it == tiles_.end()) return nullptr;
        it->second.shared = true;  // the buffer now has an owner outside this store
        return it->second.data;
    }
    // Installs `data` as this tile, marked shared: the caller still holds the pointer it
    // passed and may keep it (PaintCommand keeps every delta it applies), so the store
    // cannot assume it owns the buffer alone. The cost of being wrong the other way is a
    // silently corrupted undo step; the cost of this is one fork on the next in-place
    // write, and in-place writes are not on any hot path.
    void setTile(TileCoord c, std::shared_ptr<Tile> data) {
        const Key key = keyOf(c);
        if (data == nullptr) {
            // Only invalidate if something was actually removed, so a no-op erase
            // does not force a rescan.
            if (tiles_.erase(key) > 0) boundsDirty_ = true;
        } else {
            const bool added = !tiles_.contains(key);
            tiles_[key] = Entry{std::move(data), true};
            if (added) noteTileAdded(c);  // replacing in place cannot move the bounds
        }
    }
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }

    // Smallest document-space rect covering all occupied tiles (empty if none).
    //
    // Cached, because the compositor calls this per layer per display tile as its
    // cull test and a full map scan there costs one node visit per stored tile per
    // tile drawn. Adding a tile can only grow the bounds, so that is maintained in
    // O(1); removing one can shrink them, which cannot be, so removal marks the
    // cache stale and the next read recomputes once.
    [[nodiscard]] Rect contentBounds() const noexcept {
        if (boundsDirty_) {
            Rect bounds{};
            for (const auto& [key, entry] : tiles_) {
                (void)entry;
                bounds = bounds.united(tileBounds(TileCoord{key.first, key.second}));
            }
            bounds_ = bounds;
            boundsDirty_ = false;
        }
        return bounds_;
    }

    // A shallow copy that SHARES tiles (copy-on-write). This is the snapshot /
    // duplicate-layer mechanism: unchanged tiles stay shared until written.
    [[nodiscard]] TileStoreT shallowClone() const { return TileStoreT(*this); }

    // For testing/diagnostics: how many tiles this store uniquely owns (use_count == 1).
    // A measurement, NOT the fork trigger: see the note on `Entry::shared`.
    [[nodiscard]] std::size_t uniquelyOwnedTileCount() const noexcept {
        std::size_t n = 0;
        for (const auto& [key, entry] : tiles_) {
            (void)key;
            if (entry.data.use_count() == 1) ++n;
        }
        return n;
    }

    // How many tiles are currently marked shared, i.e. how many a write would fork.
    // For tests and for measuring the cost a snapshot imposes on subsequent painting.
    [[nodiscard]] std::size_t sharedTileCount() const noexcept {
        std::size_t n = 0;
        for (const auto& [key, entry] : tiles_) {
            (void)key;
            if (entry.shared) ++n;
        }
        return n;
    }

    template <class F>
    void forEachTile(F&& f) const {
        for (const auto& [key, entry] : tiles_) {
            f(TileCoord{key.first, key.second}, *entry.data);
        }
    }

private:
    using Key = std::pair<int, int>;  // {col, row}, ordered for std::map
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    // One stored tile, plus whether its buffer has an owner outside this store.
    //
    // `shared` is the copy-on-write fork trigger. It is deliberately NOT a refcount
    // test. shared_ptr::use_count() answers a different question ("how many owners
    // exist right now") and answers it with a value that another thread can invalidate
    // between the test and the write, which is why the previous implementation carried
    // a note saying it must be replaced before any worker touches a document. `shared`
    // answers the question that actually matters ("has this buffer ever escaped") and
    // is set by the only thread that mutates the store, before the reader exists.
    //
    // Monotone within a buffer's life: set when the pointer is handed out or copied,
    // and cleared only by installing a DIFFERENT buffer (which the escaped pointer does
    // not alias). So a stale `true` costs one unnecessary fork and can never cost
    // correctness, while a stale `false` is impossible.
    //
    // Mutable because handing a tile out (sharedTile) and copying the store
    // (shallowClone) are const operations that nonetheless change what this store is
    // allowed to do next. Only ever written by the thread that owns the store; a
    // snapshot's store is never written after it is created. See Document::snapshot.
    struct Entry {
        std::shared_ptr<Tile> data;
        mutable bool shared = false;
    };

    // Get a mutable tile, creating it if absent and forking it if shared (COW).
    //
    // PRIVATE on purpose. It is the one place a tile buffer is mutated in place, and it
    // returns a reference whose lifetime this class cannot bound: a caller holding that
    // reference across a snapshot would write into a buffer the snapshot owns, with the
    // fork already behind it. Mutation from outside goes through setPixel/fillRect/
    // setTile, each of which re-establishes the barrier on every call.
    [[nodiscard]] Tile& editable(TileCoord c) {
        const Key key = keyOf(c);
        auto it = tiles_.find(key);
        if (it == tiles_.end()) {
            it = tiles_.emplace(key, Entry{std::make_shared<Tile>(), false}).first;
            noteTileAdded(c);
            return *it->second.data;
        }
        if (it->second.shared) {
            it->second.data = std::make_shared<Tile>(*it->second.data);
            it->second.shared = false;  // the fork is private again
        }
        return *it->second.data;
    }

    // Growth is monotonic, so a new tile only ever extends the bounds. While the
    // cache is stale there is nothing to extend; the pending recompute covers it.
    void noteTileAdded(TileCoord c) noexcept {
        if (!boundsDirty_) bounds_ = bounds_.united(tileBounds(c));
    }

    // Every entry in both stores is now shared, by definition of what a copy is.
    void markAllShared() const noexcept {
        for (const auto& [key, entry] : tiles_) {
            (void)key;
            entry.shared = true;
        }
    }

    std::map<Key, Entry> tiles_;

    // Mutable so contentBounds() can stay const and noexcept. Copying the store
    // copies both, which is correct: the same tiles imply the same bounds.
    mutable Rect bounds_{};
    mutable bool boundsDirty_ = false;
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
