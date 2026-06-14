#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(tile_bounds) {
    PE_CHECK_EQ(tileBounds(TileCoord{0, 0}), (Rect{0, 0, kTileSize, kTileSize}));
    PE_CHECK_EQ(tileBounds(TileCoord{1, 2}),
                (Rect{kTileSize, 2 * kTileSize, kTileSize, kTileSize}));
}

PE_TEST(tile_floordiv_negative) {
    PE_CHECK_EQ(floorDiv(0, 256), 0);
    PE_CHECK_EQ(floorDiv(255, 256), 0);
    PE_CHECK_EQ(floorDiv(256, 256), 1);
    PE_CHECK_EQ(floorDiv(-1, 256), -1);
    PE_CHECK_EQ(floorDiv(-256, 256), -1);
    PE_CHECK_EQ(floorDiv(-257, 256), -2);
}

PE_TEST(tiles_for_rect) {
    // A single pixel at the origin touches exactly one tile.
    TileSpan one = tilesForRect(Rect{0, 0, 1, 1});
    PE_CHECK_EQ(one.count(), 1);

    // A rect spanning the seam between tile 0 and tile 1 touches two columns.
    TileSpan seam = tilesForRect(Rect{kTileSize - 1, 0, 2, 1});
    PE_CHECK_EQ(seam.colBegin, 0);
    PE_CHECK_EQ(seam.colEnd, 2);
    PE_CHECK_EQ(seam.count(), 2);

    // Empty rect touches nothing.
    PE_CHECK_EQ(tilesForRect(Rect{}).count(), 0);

    // A 2x2 tile block.
    TileSpan block = tilesForRect(Rect{0, 0, 2 * kTileSize, 2 * kTileSize});
    PE_CHECK_EQ(block.count(), 4);
}
