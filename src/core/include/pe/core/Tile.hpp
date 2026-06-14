#pragma once

#include "pe/core/Geometry.hpp"

#include <cstdint>

namespace pe {

// PhotoEdit stores layer pixels in fixed-size square tiles. Tiles are the unit
// of: dirty tracking, GPU upload, undo deltas, scratch-disk paging, and
// multithreaded filtering. See docs/systems/02-canvas-rendering.md and
// docs/systems/22-performance.md.
//
// 256 is a deliberate balance: large enough to amortize per-tile overhead,
// small enough that a single brush dab touches few tiles and GPU uploads stay
// cheap.
inline constexpr int kTileSize = 256;

// Address of a tile within a layer's tile grid (not pixel coordinates).
struct TileCoord {
    int col = 0;
    int row = 0;

    constexpr bool operator==(const TileCoord&) const = default;
};

// Pixel bounds covered by a tile at the given coordinate.
[[nodiscard]] constexpr Rect tileBounds(TileCoord c) noexcept {
    return Rect{c.col * kTileSize, c.row * kTileSize, kTileSize, kTileSize};
}

// Floor-divide that is correct for negative coordinates (layers may extend into
// negative space relative to the canvas origin).
[[nodiscard]] constexpr int floorDiv(int a, int b) noexcept {
    const int q = a / b;
    const int r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

// The inclusive range of tile coordinates a pixel rect overlaps.
struct TileSpan {
    int colBegin = 0;
    int rowBegin = 0;
    int colEnd = 0; // exclusive
    int rowEnd = 0; // exclusive

    [[nodiscard]] constexpr int count() const noexcept {
        const int cols = colEnd - colBegin;
        const int rows = rowEnd - rowBegin;
        return (cols <= 0 || rows <= 0) ? 0 : cols * rows;
    }
};

[[nodiscard]] constexpr TileSpan tilesForRect(Rect r) noexcept {
    if (r.isEmpty()) return TileSpan{};
    return TileSpan{
        floorDiv(r.left(), kTileSize),
        floorDiv(r.top(), kTileSize),
        floorDiv(r.right() - 1, kTileSize) + 1,
        floorDiv(r.bottom() - 1, kTileSize) + 1,
    };
}

} // namespace pe
