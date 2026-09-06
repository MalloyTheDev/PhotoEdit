#include "pe/core/TileStore.hpp"
#include "pe_test.hpp"

using namespace pe;

namespace {
constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};
}  // namespace

PE_TEST(tilestore_empty_reads_transparent) {
    TileStore s;
    PE_CHECK(s.empty());
    PE_CHECK_EQ(s.pixel(0, 0), (Rgba8{0, 0, 0, 0}));
    PE_CHECK_EQ(s.pixel(-100, 50), (Rgba8{0, 0, 0, 0}));
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(0));
}

PE_TEST(tilestore_set_get_including_negative) {
    TileStore s;
    s.setPixel(5, 7, kRed);
    s.setPixel(-1, -1, kBlue);  // lands in tile (-1,-1), local (255,255)
    PE_CHECK_EQ(s.pixel(5, 7), kRed);
    PE_CHECK_EQ(s.pixel(-1, -1), kBlue);
    PE_CHECK_EQ(s.pixel(6, 7), (Rgba8{0, 0, 0, 0}));  // untouched neighbor
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(2));
}

PE_TEST(tilestore_content_bounds) {
    TileStore s;
    PE_CHECK(s.contentBounds().isEmpty());
    s.setPixel(10, 10, kRed);  // tile (0,0)
    PE_CHECK_EQ(s.contentBounds(), (Rect{0, 0, kTileSize, kTileSize}));
    s.setPixel(kTileSize + 1, 1, kRed);  // tile (1,0)
    PE_CHECK_EQ(s.contentBounds(), (Rect{0, 0, 2 * kTileSize, kTileSize}));
}

PE_TEST(tilestore_copy_on_write) {
    TileStore a;
    a.setPixel(5, 5, kRed);
    PE_CHECK_EQ(a.uniquelyOwnedTileCount(), static_cast<std::size_t>(1));

    TileStore b = a.shallowClone();  // shares the tile
    PE_CHECK_EQ(b.pixel(5, 5), kRed);
    PE_CHECK_EQ(a.uniquelyOwnedTileCount(), static_cast<std::size_t>(0));  // shared
    PE_CHECK_EQ(b.uniquelyOwnedTileCount(), static_cast<std::size_t>(0));

    b.setPixel(5, 5, kBlue);            // forks the shared tile (COW)
    PE_CHECK_EQ(a.pixel(5, 5), kRed);   // original untouched
    PE_CHECK_EQ(b.pixel(5, 5), kBlue);  // clone changed
    PE_CHECK_EQ(a.uniquelyOwnedTileCount(), static_cast<std::size_t>(1));  // unique again
    PE_CHECK_EQ(b.uniquelyOwnedTileCount(), static_cast<std::size_t>(1));
}

PE_TEST(tilestore_fill_rect) {
    TileStore s;
    s.fillRect(Rect{0, 0, 3, 2}, kRed);
    PE_CHECK_EQ(s.pixel(0, 0), kRed);
    PE_CHECK_EQ(s.pixel(2, 1), kRed);
    PE_CHECK_EQ(s.pixel(3, 0), (Rgba8{0, 0, 0, 0}));  // exclusive right edge
    PE_CHECK_EQ(s.pixel(0, 2), (Rgba8{0, 0, 0, 0}));  // exclusive bottom edge
}

PE_TEST(tilestore16_high_depth_storage) {
    // The 16-bit store (TileStore16 = TileStoreT<Rgba16>) shares all the proven
    // sparse/CoW machinery, now carrying full 16-bit precision per channel.
    TileStore16 s;
    PE_CHECK(s.empty());
    PE_CHECK_EQ(s.pixel(0, 0), (Rgba16{0, 0, 0, 0}));  // absent -> transparent

    const Rgba16 deep{40000, 200, 65535, 65535};  // a value with no 8-bit equivalent
    s.setPixel(5, 5, deep);
    s.setPixel(-3, -7, deep);  // negative coords land in the correct tile
    PE_CHECK_EQ(s.pixel(5, 5), deep);
    PE_CHECK_EQ(s.pixel(-3, -7), deep);
    PE_CHECK_EQ(s.pixel(6, 6), (Rgba16{0, 0, 0, 0}));  // untouched neighbor
}

PE_TEST(tilestore16_copy_on_write) {
    TileStore16 a;
    const Rgba16 v1{1000, 2000, 3000, 65535};
    const Rgba16 v2{9000, 8000, 7000, 65535};
    a.setPixel(5, 5, v1);

    TileStore16 b = a.shallowClone();  // shares the tile
    PE_CHECK_EQ(b.pixel(5, 5), v1);
    PE_CHECK_EQ(a.uniquelyOwnedTileCount(), static_cast<std::size_t>(0));  // shared

    b.setPixel(5, 5, v2);            // forks the shared tile (COW)
    PE_CHECK_EQ(a.pixel(5, 5), v1);  // original untouched
    PE_CHECK_EQ(b.pixel(5, 5), v2);  // clone changed
    PE_CHECK_EQ(a.uniquelyOwnedTileCount(), static_cast<std::size_t>(1));
}

PE_TEST(tilestoref_float_storage) {
    // The float store preserves out-of-[0,1] / HDR values verbatim.
    TileStoreF s;
    const Rgbaf hdr{2.5f, 0.5f, -0.1f, 1.0f};
    s.setPixel(10, 10, hdr);
    const Rgbaf got = s.pixel(10, 10);
    PE_CHECK_NEAR(got.r, 2.5f);
    PE_CHECK_NEAR(got.b, -0.1f);
    PE_CHECK_NEAR(s.pixel(0, 0).a, 0.0f);  // absent -> transparent
}

// ---------------------------------------------------------------------------
// contentBounds() invariants.
//
// These pin the OBSERVABLE contract independently of how it is computed, so a
// cached implementation cannot drift from a recomputed one. The dangerous case is
// erase: adding a tile can only grow the bounds (cheap to maintain incrementally),
// but removing one can shrink them, and a bound left stale after an erase would
// silently over-report content and cause the compositor to cull nothing, or, if
// wrong in the other direction, to cull real content.
// ---------------------------------------------------------------------------

namespace {
// The bounds recomputed from scratch, as the reference the cache must match.
Rect referenceBounds(const TileStore& s) {
    Rect b{};
    for (int row = -4; row <= 4; ++row) {
        for (int col = -4; col <= 4; ++col) {
            const TileCoord c{col, row};
            if (s.hasTileAt(c)) b = b.united(tileBounds(c));
        }
    }
    return b;
}
}  // namespace

PE_TEST(tilestore_content_bounds_matches_a_recomputed_reference) {
    TileStore s;
    PE_CHECK(s.contentBounds().isEmpty());
    PE_CHECK(s.contentBounds() == referenceBounds(s));

    s.setPixel(5, 5, kRed);  // tile (0,0)
    PE_CHECK(s.contentBounds() == referenceBounds(s));

    s.setPixel(-1, -1, kBlue);  // tile (-1,-1)
    PE_CHECK(s.contentBounds() == referenceBounds(s));

    s.setPixel(kTileSize * 2 + 3, kTileSize + 1, kRed);  // tile (2,1)
    PE_CHECK(s.contentBounds() == referenceBounds(s));
}

PE_TEST(tilestore_content_bounds_shrinks_after_a_tile_is_removed) {
    // The case a naively cached bound gets wrong: growth is monotonic, removal is not.
    TileStore s;
    s.setPixel(5, 5, kRed);                                  // tile (0,0)
    s.setPixel(kTileSize * 3 + 5, kTileSize * 3 + 5, kRed);  // tile (3,3)
    const Rect wide = s.contentBounds();
    PE_CHECK_EQ(wide.width, kTileSize * 4);
    PE_CHECK_EQ(wide.height, kTileSize * 4);

    s.setTile(TileCoord{3, 3}, nullptr);  // remove the far tile
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(1));
    PE_CHECK(s.contentBounds() == referenceBounds(s));
    PE_CHECK_EQ(s.contentBounds().width, kTileSize);  // must have SHRUNK, not stayed wide

    s.setTile(TileCoord{0, 0}, nullptr);  // remove the last one
    PE_CHECK(s.empty());
    PE_CHECK(s.contentBounds().isEmpty());
}

PE_TEST(tilestore_content_bounds_after_remove_then_add) {
    // Interleaving is where a dirty-flag scheme goes wrong if the flag is cleared
    // or applied in the wrong order.
    TileStore s;
    s.setPixel(kTileSize * 2 + 1, 1, kRed);  // tile (2,0)
    s.setPixel(1, 1, kRed);                  // tile (0,0)
    s.setTile(TileCoord{2, 0}, nullptr);
    PE_CHECK(s.contentBounds() == referenceBounds(s));

    s.setPixel(kTileSize * 3 + 1, 1, kRed);  // tile (3,0), wider than before
    PE_CHECK(s.contentBounds() == referenceBounds(s));
    PE_CHECK_EQ(s.contentBounds().width, kTileSize * 4);

    s.setTile(TileCoord{3, 0}, nullptr);
    s.setTile(TileCoord{0, 0}, nullptr);
    PE_CHECK(s.empty());
    PE_CHECK(s.contentBounds() == referenceBounds(s));
}

PE_TEST(tilestore_content_bounds_unchanged_by_replace_cow_and_noop_erase) {
    TileStore s;
    s.setPixel(5, 5, kRed);
    const Rect before = s.contentBounds();

    // Replacing an existing tile's data does not move it.
    s.setTile(TileCoord{0, 0}, s.sharedTile(TileCoord{0, 0}));
    PE_CHECK(s.contentBounds() == before);

    // Erasing a tile that was never there must not disturb anything.
    s.setTile(TileCoord{9, 9}, nullptr);
    PE_CHECK(s.contentBounds() == before);
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(1));

    // A copy-on-write fork replaces the tile value, not its coordinate.
    TileStore snap = s.shallowClone();
    s.setPixel(6, 6, kBlue);  // forks tile (0,0)
    PE_CHECK(s.contentBounds() == before);
    PE_CHECK(snap.contentBounds() == before);
}

PE_TEST(tilestore_content_bounds_survives_a_shallow_clone) {
    TileStore s;
    s.setPixel(5, 5, kRed);
    s.setPixel(kTileSize * 2 + 5, kTileSize + 5, kBlue);
    const Rect expected = s.contentBounds();

    TileStore clone = s.shallowClone();
    PE_CHECK(clone.contentBounds() == expected);
    PE_CHECK(clone.contentBounds() == referenceBounds(clone));

    // The clone is independent: removing from it must not affect the original.
    clone.setTile(TileCoord{2, 1}, nullptr);
    PE_CHECK(clone.contentBounds() == referenceBounds(clone));
    PE_CHECK(s.contentBounds() == expected);
}

PE_TEST(tilestore_content_bounds_after_fill_rect_spanning_tiles) {
    TileStore s;
    s.fillRect(Rect{kTileSize - 2, kTileSize - 2, 6, 6}, kRed);  // straddles four tiles
    PE_CHECK_EQ(s.tileCount(), static_cast<std::size_t>(4));
    PE_CHECK(s.contentBounds() == referenceBounds(s));
    PE_CHECK_EQ(s.contentBounds().width, kTileSize * 2);
}
