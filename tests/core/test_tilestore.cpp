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
