#include "pe/core/TileStore.hpp"

namespace pe {

namespace {

// Local pixel offset within a tile for a document coordinate. Correct for
// negative coordinates: the result is always in [0, kTileSize).
inline int localIndex(int coord) noexcept {
    int m = coord % kTileSize;
    if (m < 0) m += kTileSize;
    return m;
}

}  // namespace

Rgba8 TileStore::pixel(int x, int y) const noexcept {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    const TileData* t = find(c);
    if (t == nullptr) return Rgba8{};  // transparent
    return t->at(localIndex(x), localIndex(y));
}

void TileStore::setPixel(int x, int y, Rgba8 c) {
    const TileCoord coord{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    TileData& t = editable(coord);
    t.set(localIndex(x), localIndex(y), c);
}

void TileStore::fillRect(Rect r, Rgba8 c) {
    if (r.isEmpty()) return;
    for (int y = r.top(); y < r.bottom(); ++y) {
        for (int x = r.left(); x < r.right(); ++x) {
            setPixel(x, y, c);
        }
    }
}

const TileData* TileStore::find(TileCoord c) const noexcept {
    auto it = tiles_.find(keyOf(c));
    return it == tiles_.end() ? nullptr : it->second.get();
}

bool TileStore::hasTileAt(TileCoord c) const noexcept {
    return tiles_.find(keyOf(c)) != tiles_.end();
}

std::shared_ptr<TileData> TileStore::sharedTile(TileCoord c) const {
    auto it = tiles_.find(keyOf(c));
    return it == tiles_.end() ? nullptr : it->second;
}

void TileStore::setTile(TileCoord c, std::shared_ptr<TileData> data) {
    const Key key = keyOf(c);
    if (data == nullptr) {
        tiles_.erase(key);
    } else {
        tiles_[key] = std::move(data);
    }
}

TileData& TileStore::editable(TileCoord c) {
    const Key key = keyOf(c);
    auto it = tiles_.find(key);
    if (it == tiles_.end()) {
        auto data = std::make_shared<TileData>();
        it = tiles_.emplace(key, std::move(data)).first;
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
        it->second = std::make_shared<TileData>(*it->second);
    }
    return *it->second;
}

Rect TileStore::contentBounds() const noexcept {
    Rect bounds{};
    for (const auto& [key, data] : tiles_) {
        (void)data;
        bounds = bounds.united(tileBounds(TileCoord{key.first, key.second}));
    }
    return bounds;
}

std::size_t TileStore::uniquelyOwnedTileCount() const noexcept {
    std::size_t n = 0;
    for (const auto& [key, data] : tiles_) {
        (void)key;
        if (data.use_count() == 1) ++n;
    }
    return n;
}

}  // namespace pe
