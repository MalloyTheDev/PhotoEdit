// The copy-on-write barrier is only as complete as the list of ways a tile buffer can
// leave a store.
//
// TileStoreT no longer forks on `use_count() > 1`; it forks on an `Entry::shared` flag set
// when a buffer escapes. That is a better contract, but it moves the risk: a refcount is
// automatic, whereas a flag has to be set by every escape path. A path that hands a buffer
// out without marking is exactly as bad as the racy refcount test it replaced, and it fails
// silently, so the escape surface is enumerated here rather than trusted.
//
// The surface, as of this file, is four producers and two read accessors:
//
//   sharedTile()        hands out a shared_ptr           -> must mark
//   copy constructor    duplicates every entry           -> must mark, both sides
//   copy assignment     the same                         -> must mark, both sides
//   setTile()           installs a caller's pointer      -> must mark
//   find()              hands out a raw const Tile*      -> lifetime rule, see below
//   forEachTile()       hands out a const Tile& per call -> callback-scoped
//
// Every producer is checked below against every in-place mutation. If a new way to obtain a
// tile buffer is added to TileStoreT, it belongs in this file, and the header says so.

#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/TileStore.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

using namespace pe;

namespace {

constexpr Rgba8 kOriginal{11, 22, 33, 255};
constexpr Rgba8 kWritten{200, 100, 50, 255};

// Two tiles across and two down, so a mistake that happens to work on tile (0,0) still has
// somewhere to show up.
TileStore twoByTwo() {
    TileStore s;
    s.fillRect(Rect{0, 0, 2 * kTileSize, 2 * kTileSize}, kOriginal);
    return s;
}

// The coordinates every case pokes, one per tile.
constexpr TileCoord kCoords[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};

int pixelX(TileCoord c) {
    return c.col * kTileSize + 7;
}
int pixelY(TileCoord c) {
    return c.row * kTileSize + 9;
}

}  // namespace

PE_TEST(cow_escape_via_sharedtile_survives_every_in_place_write) {
    // An undo delta's `before` pointer. If a later write landed in it, undo would restore
    // the post-stroke pixels and the edit would be unundoable.
    for (const TileCoord c : kCoords) {
        TileStore s = twoByTwo();
        const std::shared_ptr<TileStore::Tile> escaped = s.sharedTile(c);
        PE_CHECK(escaped != nullptr);
        if (escaped == nullptr) continue;

        s.setPixel(pixelX(c), pixelY(c), kWritten);
        PE_CHECK(escaped->at(7, 9) == kOriginal);
        PE_CHECK(s.find(c)->at(7, 9) == kWritten);
        PE_CHECK(s.find(c) != escaped.get());

        // And fillRect, which is the other in-place path.
        const std::shared_ptr<TileStore::Tile> escaped2 = s.sharedTile(c);
        s.fillRect(Rect{pixelX(c), pixelY(c), 3, 3}, Rgba8{1, 2, 3, 255});
        PE_CHECK(escaped2->at(7, 9) == kWritten);
        PE_CHECK(s.find(c)->at(7, 9) == Rgba8{1, 2, 3, 255});
    }
}

PE_TEST(cow_escape_via_the_copy_constructor_isolates_both_sides) {
    // A snapshot, and a duplicated layer. Both sides must fork, not just the original: the
    // copy is a store in its own right and can be written to first.
    for (std::size_t i = 0; i < 4; ++i) {
        const TileCoord c = kCoords[i];
        TileStore original = twoByTwo();
        TileStore copy(original);  // NOLINT: exercising the copy ctor on purpose

        original.setPixel(pixelX(c), pixelY(c), kWritten);
        PE_CHECK(copy.find(c)->at(7, 9) == kOriginal);
        PE_CHECK(original.find(c)->at(7, 9) == kWritten);

        // Now the other direction, on a tile neither has written yet.
        const TileCoord other = kCoords[(i + 1) % 4];
        copy.setPixel(pixelX(other), pixelY(other), Rgba8{5, 5, 5, 255});
        PE_CHECK(original.find(other)->at(7, 9) == kOriginal);
        PE_CHECK(copy.find(other)->at(7, 9) == Rgba8{5, 5, 5, 255});
    }
}

PE_TEST(cow_escape_via_copy_assignment_isolates_both_sides) {
    // Distinct from the copy constructor: assignment overwrites an existing store, and a
    // defaulted operator= would leave both sides believing they owned their buffers.
    for (const TileCoord c : kCoords) {
        TileStore original = twoByTwo();
        TileStore target;
        target.fillRect(Rect{0, 0, 16, 16}, Rgba8{9, 9, 9, 255});  // has content of its own
        target = original;

        target.setPixel(pixelX(c), pixelY(c), kWritten);
        PE_CHECK(original.find(c)->at(7, 9) == kOriginal);
        PE_CHECK(target.find(c)->at(7, 9) == kWritten);

        original.setPixel(pixelX(c), pixelY(c) + 1, Rgba8{4, 4, 4, 255});
        PE_CHECK(target.find(c)->at(7, 10) == kOriginal);
    }
}

PE_TEST(cow_escape_via_settile_survives_every_in_place_write) {
    // PaintCommand keeps every delta it applies, so the pointer it hands to setTile is
    // still live afterwards. The store cannot assume it owns that buffer alone.
    for (const TileCoord c : kCoords) {
        TileStore s = twoByTwo();
        auto installed = std::make_shared<TileStore::Tile>();
        installed->set(7, 9, kOriginal);
        s.setTile(c, installed);
        PE_CHECK(s.find(c) == installed.get());

        s.setPixel(pixelX(c), pixelY(c), kWritten);
        PE_CHECK(installed->at(7, 9) == kOriginal);  // the caller's copy is untouched
        PE_CHECK(s.find(c) != installed.get());
        PE_CHECK(s.find(c)->at(7, 9) == kWritten);
    }
}

PE_TEST(cow_a_forked_tile_is_private_again_so_painting_does_not_copy_repeatedly) {
    // The cost side. If the flag were never cleared, every write to a tile that had once
    // been snapshotted would copy 256 KB, and a stroke over a saved document would be
    // dramatically slower than the same stroke over an unsaved one.
    TileStore s = twoByTwo();
    const std::shared_ptr<TileStore::Tile> escaped = s.sharedTile(TileCoord{1, 1});
    PE_CHECK_EQ(s.sharedTileCount(), static_cast<std::size_t>(1));

    s.setPixel(pixelX(TileCoord{1, 1}), pixelY(TileCoord{1, 1}), kWritten);
    const TileStore::Tile* forked = s.find(TileCoord{1, 1});
    PE_CHECK_EQ(s.sharedTileCount(), static_cast<std::size_t>(0));
    for (int i = 0; i < 128; ++i) {
        s.setPixel(kTileSize + i, kTileSize + i, Rgba8{static_cast<std::uint8_t>(i), 0, 0, 255});
    }
    PE_CHECK(s.find(TileCoord{1, 1}) == forked);  // 128 more writes, no further fork
    PE_CHECK(escaped->at(7, 9) == kOriginal);     // and the escape is still intact
}

PE_TEST(cow_a_raw_tile_pointer_stays_valid_while_a_shared_owner_holds_it) {
    // find() hands out a raw pointer into a reference-counted buffer, which is the one
    // escape the flag cannot protect: it is a LIFETIME question, not a mutation question.
    // The rule the header states is that a find() result must not be cached across a
    // mutation of the same store. What callers may rely on is this: while some owner holds
    // the shared_ptr, the buffer a fork left behind is still there to read.
    TileStore s = twoByTwo();
    const std::shared_ptr<TileStore::Tile> keepAlive = s.sharedTile(TileCoord{0, 1});
    const TileStore::Tile* raw = s.find(TileCoord{0, 1});
    PE_CHECK(raw == keepAlive.get());

    s.setPixel(pixelX(TileCoord{0, 1}), pixelY(TileCoord{0, 1}), kWritten);  // forks
    PE_CHECK(s.find(TileCoord{0, 1}) != raw);                                // the store moved on
    PE_CHECK(raw->at(7, 9) == kOriginal);  // the old buffer is alive and unchanged

    // Erasing the entry does not free it either, for the same reason.
    s.setTile(TileCoord{0, 1}, nullptr);
    PE_CHECK(s.find(TileCoord{0, 1}) == nullptr);
    PE_CHECK(keepAlive->at(7, 9) == kOriginal);
}

PE_TEST(cow_moving_a_store_transfers_the_shared_marks_with_it) {
    // A move is not an escape, but it must not LOSE a mark either: a moved-to store holding
    // a buffer someone else still owns has to keep forking.
    TileStore source = twoByTwo();
    const std::shared_ptr<TileStore::Tile> escaped = source.sharedTile(TileCoord{1, 0});
    TileStore moved = std::move(source);
    PE_CHECK_EQ(moved.sharedTileCount(), static_cast<std::size_t>(1));

    moved.setPixel(pixelX(TileCoord{1, 0}), pixelY(TileCoord{1, 0}), kWritten);
    PE_CHECK(escaped->at(7, 9) == kOriginal);
    PE_CHECK(moved.find(TileCoord{1, 0})->at(7, 9) == kWritten);
}

PE_TEST(cow_holds_for_the_sixteen_bit_and_float_stores_too) {
    // The barrier lives in the template, but the three instantiations are what documents
    // and undo actually use, and a mistake made per-type is a real possibility.
    TileStore16 s16;
    s16.fillRect(Rect{0, 0, 2 * kTileSize, 16}, Rgba16{100, 200, 300, 65535});
    const std::shared_ptr<TileStore16::Tile> e16 = s16.sharedTile(TileCoord{1, 0});
    s16.setPixel(kTileSize + 7, 9, Rgba16{1, 1, 1, 1});
    PE_CHECK(e16->at(7, 9) == Rgba16{100, 200, 300, 65535});
    PE_CHECK(s16.find(TileCoord{1, 0})->at(7, 9) == Rgba16{1, 1, 1, 1});

    // Rgbaf has no operator== on purpose (float equality), so compare the channels the
    // write actually set. These are exact values, not the result of any arithmetic.
    TileStoreF sf;
    sf.fillRect(Rect{0, 0, 2 * kTileSize, 16}, Rgbaf{0.25f, 0.5f, 0.75f, 1.0f});
    const std::shared_ptr<TileStoreF::Tile> ef = sf.sharedTile(TileCoord{1, 0});
    sf.setPixel(kTileSize + 7, 9, Rgbaf{0.0f, 0.0f, 0.0f, 0.0f});
    const Rgbaf kept = ef->at(7, 9);
    PE_CHECK(kept.r == 0.25f && kept.g == 0.5f && kept.b == 0.75f && kept.a == 1.0f);
    const Rgbaf now = sf.find(TileCoord{1, 0})->at(7, 9);
    PE_CHECK(now.r == 0.0f && now.g == 0.0f && now.b == 0.0f && now.a == 0.0f);
}
