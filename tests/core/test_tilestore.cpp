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
