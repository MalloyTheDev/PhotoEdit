#include "pe/core/Selection.hpp"

#include <algorithm>
#include <cstddef>

namespace pe {

namespace {
// Bound per-operation pixel iteration (a selection fill over an absurd rect must
// not hang/OOM). Matches the whole-image composite budget in spirit.
constexpr int64_t kMaxSelectionPixels = 64'000'000;

int localIndex(int coord) noexcept {
    int m = coord % kTileSize;
    if (m < 0) m += kTileSize;
    return m;
}

bool tooLarge(Rect r) noexcept {
    const int64_t area = static_cast<int64_t>(r.width) * static_cast<int64_t>(r.height);
    return area > kMaxSelectionPixels;
}
}  // namespace

uint8_t Selection::stored(int x, int y) const noexcept {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = tiles_.find(keyOf(c));
    if (it == tiles_.end()) return 0;
    return it->second[static_cast<std::size_t>(localIndex(y)) * kTileSize +
                      static_cast<std::size_t>(localIndex(x))];
}

void Selection::setValue(int x, int y, uint8_t v) {
    const TileCoord c{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    GrayTile& tile = tiles_[keyOf(c)];  // creates a zeroed tile if absent
    tile[static_cast<std::size_t>(localIndex(y)) * kTileSize +
         static_cast<std::size_t>(localIndex(x))] = v;
}

void Selection::fillRect(Rect r, uint8_t v) {
    if (r.isEmpty() || tooLarge(r)) return;
    for (int y = r.top(); y < r.bottom(); ++y) {
        for (int x = r.left(); x < r.right(); ++x) {
            setValue(x, y, v);
        }
    }
}

float Selection::coverage(int x, int y) const noexcept {
    if (!active_) return 1.0f;
    return static_cast<float>(stored(x, y)) / 255.0f;
}

uint8_t Selection::value(int x, int y) const noexcept {
    if (!active_) return 255;
    return stored(x, y);
}

void Selection::selectNone() noexcept {
    tiles_.clear();
    active_ = false;
}

void Selection::selectAll(Rect canvas) {
    tiles_.clear();
    active_ = true;
    fillRect(canvas, 255);
}

void Selection::selectRect(Rect r) {
    tiles_.clear();
    active_ = true;
    fillRect(r, 255);
}

void Selection::addRect(Rect r) {
    active_ = true;
    fillRect(r, 255);
}

void Selection::subtractRect(Rect r) {
    if (!active_) return;  // nothing selected to subtract from
    fillRect(r, 0);
}

void Selection::intersectRect(Rect r) {
    if (!active_) {
        // Intersecting an "all" selection with a rect == selecting that rect.
        selectRect(r);
        return;
    }
    // Zero every stored pixel outside r (bounded by the stored tiles).
    for (auto& [key, tile] : tiles_) {
        const TileCoord coord{key.first, key.second};
        const Rect tb = tileBounds(coord);
        for (int ly = 0; ly < kTileSize; ++ly) {
            for (int lx = 0; lx < kTileSize; ++lx) {
                const int dx = tb.left() + lx;
                const int dy = tb.top() + ly;
                if (!r.contains(Point{dx, dy})) {
                    tile[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] =
                        0;
                }
            }
        }
    }
}

void Selection::invert(Rect canvas) {
    if (canvas.isEmpty() || tooLarge(canvas)) return;
    active_ = true;
    for (int y = canvas.top(); y < canvas.bottom(); ++y) {
        for (int x = canvas.left(); x < canvas.right(); ++x) {
            setValue(x, y, static_cast<uint8_t>(255 - stored(x, y)));
        }
    }
}

Rect Selection::selectedBounds() const noexcept {
    Rect bounds{};
    for (const auto& [key, tile] : tiles_) {
        (void)tile;
        bounds = bounds.united(tileBounds(TileCoord{key.first, key.second}));
    }
    return bounds;
}

}  // namespace pe
