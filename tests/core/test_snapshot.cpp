// Copy-on-write forking and document snapshots.
//
// A save runs on a worker thread while the user keeps painting, so the bytes the worker
// serializes must be a stable point in time. The mechanism is structural sharing: a
// snapshot copies tile POINTERS, and the live document forks a tile before writing it.
//
// The property under test is byte isolation, not a reference count. A refcount is the
// wrong thing to assert on: it answers "how many owners exist right now", which is not
// the question the barrier asks and is not stable under a second thread. These tests
// therefore compare pixels, and check aliasing by tile address rather than by use_count.

#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/TileStore.hpp"
#include "pe_test.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace pe;

namespace {

// Per-tile colour, so a wrong tile is visible rather than averaged away. Per the standing
// rule for tiled semantics, every case here spans more than one tile.
Rgba8 colorFor(int col, int row) {
    return Rgba8{static_cast<std::uint8_t>(10 + 30 * col), static_cast<std::uint8_t>(10 + 30 * row),
                 static_cast<std::uint8_t>(200 - 10 * (col + row)), 255};
}

std::unique_ptr<Document> tiledDoc(int cols, int rows) {
    auto doc = Document::createBlank(Size{cols * kTileSize, rows * kTileSize});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            pl->tiles().fillRect(Rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize},
                                 colorFor(col, row));
        }
    }
    return doc;
}

const TileStore& storeOf(const Document& doc, LayerId id) {
    return static_cast<const PixelLayer*>(doc.findLayer(id))->tiles();
}

TileStore& mutableStoreOf(Document& doc, LayerId id) {
    return static_cast<PixelLayer*>(doc.findLayer(id))->tiles();
}

}  // namespace

PE_TEST(snapshot_shares_tile_buffers_rather_than_copying_pixels) {
    // The reason a snapshot is affordable at all: it copies pointers, not the 12 MB of
    // pixels a document this size holds. If this ever became a deep copy, taking one on
    // the GUI thread would cost more than the save it was meant to unblock.
    auto doc = tiledDoc(4, 3);
    const LayerId id = doc->activeLayer();
    const auto snap = doc->snapshot();
    PE_CHECK(snap != nullptr);
    if (snap == nullptr) return;

    const TileStore& live = storeOf(*doc, id);
    const TileStore& shot = storeOf(*snap, id);
    PE_CHECK_EQ(shot.tileCount(), live.tileCount());
    PE_CHECK_EQ(live.tileCount(), static_cast<std::size_t>(12));

    // Every tile is the same buffer on both sides...
    std::size_t aliased = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            const TileCoord c{col, row};
            if (live.find(c) != nullptr && live.find(c) == shot.find(c)) ++aliased;
        }
    }
    PE_CHECK_EQ(aliased, static_cast<std::size_t>(12));
    // ...and both stores know it, which is what makes the next write fork.
    PE_CHECK_EQ(live.sharedTileCount(), static_cast<std::size_t>(12));
    PE_CHECK_EQ(shot.sharedTileCount(), static_cast<std::size_t>(12));
}

PE_TEST(a_live_edit_forks_only_the_tiles_it_touches_and_leaves_the_snapshot_alone) {
    // The golden isolation case. Both halves of copy-on-write have to hold: the tile that
    // was written is forked, and the tiles that were not are still shared. Proving only
    // the first would permit a deep copy; proving only the second would permit corruption.
    auto doc = tiledDoc(4, 3);
    const LayerId id = doc->activeLayer();
    const auto snap = doc->snapshot();
    PE_CHECK(snap != nullptr);
    if (snap == nullptr) return;

    const TileCoord touched{1, 1};
    const TileCoord untouched{3, 2};
    const TileStore::Tile* snapTouchedBefore = storeOf(*snap, id).find(touched);
    const TileStore::Tile* liveTouchedBefore = storeOf(*doc, id).find(touched);
    PE_CHECK(snapTouchedBefore != nullptr && snapTouchedBefore == liveTouchedBefore);
    if (snapTouchedBefore == nullptr) return;

    // The bytes both sides start from.
    const Rgba8 original = colorFor(touched.col, touched.row);
    PE_CHECK(snapTouchedBefore->at(5, 5) == original);

    // A REAL committed document operation, not a poke at the tile array: the barrier has
    // to hold on the path the application actually takes.
    auto cmd = bucketFill(*doc, id, touched.col * kTileSize + 5, touched.row * kTileSize + 5,
                          Rgbaf{1.0f, 0.04f, 0.04f, 1.0f}, 0, nullptr);
    PE_CHECK(cmd != nullptr);
    if (cmd == nullptr) return;
    doc->history().push(std::move(cmd));

    const TileStore& live = storeOf(*doc, id);
    const TileStore& shot = storeOf(*snap, id);

    // 1. The live tile really changed.
    PE_CHECK(live.find(touched) != nullptr);
    if (live.find(touched) == nullptr) return;
    PE_CHECK(!(live.find(touched)->at(5, 5) == original));
    PE_CHECK(live.find(touched)->at(5, 5).r > 200);  // and changed to the colour asked for
    // 2. The snapshot's bytes did not.
    PE_CHECK(shot.find(touched) != nullptr);
    PE_CHECK(shot.find(touched)->at(5, 5) == original);
    PE_CHECK(shot.find(touched) == snapTouchedBefore);  // same buffer it always had
    // 3. And they are no longer the same buffer, which is what makes 1 and 2 compatible.
    PE_CHECK(live.find(touched) != shot.find(touched));
    // 4. A tile the fill did not reach is still shared, so this was a fork and not a copy.
    PE_CHECK(live.find(untouched) != nullptr);
    PE_CHECK(live.find(untouched) == shot.find(untouched));

    // The application reaches tiles two different ways and only one of them goes through
    // the write barrier, so both have to be checked here or a broken barrier survives.
    // A paint commit REPLACES a tile pointer (setTile), which is safe whatever the barrier
    // does. An in-place write (setPixel, fillRect, the deserializer) mutates the buffer,
    // and that is the one the fork exists for.
    const TileStore::Tile* snapUntouchedBefore = shot.find(untouched);
    const Rgba8 untouchedOriginal = snapUntouchedBefore->at(9, 9);
    mutableStoreOf(*doc, id).setPixel(untouched.col * kTileSize + 9, untouched.row * kTileSize + 9,
                                      Rgba8{1, 2, 3, 255});
    PE_CHECK(live.find(untouched)->at(9, 9) == Rgba8{1, 2, 3, 255});  // the write landed
    PE_CHECK(shot.find(untouched) == snapUntouchedBefore);            // same buffer
    PE_CHECK(shot.find(untouched)->at(9, 9) == untouchedOriginal);    // untouched bytes
    PE_CHECK(live.find(untouched) != shot.find(untouched));           // forked
}

PE_TEST(writing_a_uniquely_owned_tile_does_not_clone_it) {
    // The other half of the bargain. If every write forked, painting would copy 256 KB per
    // touched tile per stroke and the barrier would be a performance bug rather than a
    // correctness one.
    auto doc = tiledDoc(2, 2);
    const LayerId id = doc->activeLayer();
    TileStore& live = mutableStoreOf(*doc, id);
    PE_CHECK_EQ(live.sharedTileCount(), static_cast<std::size_t>(0));

    const TileStore::Tile* before = live.find(TileCoord{0, 0});
    for (int i = 0; i < 64; ++i) live.setPixel(i, i, Rgba8{1, 2, 3, 255});
    PE_CHECK(live.find(TileCoord{0, 0}) == before);  // 64 writes, no fork
    PE_CHECK_EQ(live.sharedTileCount(), static_cast<std::size_t>(0));

    // And a tile created by a write is unshared from the start.
    live.setPixel(5 * kTileSize, 5 * kTileSize, Rgba8{9, 9, 9, 255});
    const TileStore::Tile* fresh = live.find(TileCoord{5, 5});
    PE_CHECK(fresh != nullptr);
    live.setPixel(5 * kTileSize + 1, 5 * kTileSize + 1, Rgba8{8, 8, 8, 255});
    PE_CHECK(live.find(TileCoord{5, 5}) == fresh);
}

PE_TEST(handing_a_tile_out_marks_it_shared_so_the_next_write_forks) {
    // sharedTile() is how an undo delta keeps the previous bytes. If handing the pointer
    // out did not arm the barrier, the next in-place write would edit the undo step and a
    // stroke would undo to the wrong pixels.
    auto doc = tiledDoc(2, 2);
    const LayerId id = doc->activeLayer();
    TileStore& live = mutableStoreOf(*doc, id);

    const std::shared_ptr<TileStore::Tile> delta = live.sharedTile(TileCoord{0, 0});
    PE_CHECK(delta != nullptr);
    if (delta == nullptr) return;
    const Rgba8 original = delta->at(3, 3);
    PE_CHECK_EQ(live.sharedTileCount(), static_cast<std::size_t>(1));

    live.setPixel(3, 3, Rgba8{77, 88, 99, 255});
    PE_CHECK(live.find(TileCoord{0, 0}) != delta.get());  // forked
    PE_CHECK(delta->at(3, 3) == original);                // the delta is intact
    PE_CHECK(live.find(TileCoord{0, 0})->at(3, 3) == Rgba8{77, 88, 99, 255});
    // The fork is private again, so the write after it is free.
    const TileStore::Tile* forked = live.find(TileCoord{0, 0});
    live.setPixel(4, 4, Rgba8{1, 1, 1, 255});
    PE_CHECK(live.find(TileCoord{0, 0}) == forked);
}

PE_TEST(snapshot_captures_the_metadata_a_save_reads_and_keeps_layer_identity) {
    // A snapshot that renumbered its layers would serialize with no active-layer marker,
    // and any id a caller already held would resolve to nothing in it.
    auto doc =
        Document::createBlank(Size{2 * kTileSize, kTileSize}, ColorMode::RGB, BitDepth::U8, 300);
    PE_CHECK(doc != nullptr);
    auto group = std::make_unique<GroupLayer>("Set");
    auto inner = std::make_unique<PixelLayer>("Inner", BitDepth::U8);
    const LayerId innerId = inner->id();
    inner->setOpacity(0.25f);
    inner->tiles().fillRect(Rect{0, 0, kTileSize + 40, 60}, Rgba8{5, 6, 7, 255});
    group->addChild(std::move(inner));
    const LayerId groupId = group->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
    doc->setActiveLayer(innerId);

    const auto snap = doc->snapshot();
    PE_CHECK(snap != nullptr);
    if (snap == nullptr) return;

    PE_CHECK_EQ(snap->canvasSize().width, doc->canvasSize().width);
    PE_CHECK_EQ(snap->canvasSize().height, doc->canvasSize().height);
    PE_CHECK_EQ(snap->resolutionPpi(), 300);
    PE_CHECK(snap->colorMode() == doc->colorMode());
    PE_CHECK(snap->bitDepth() == doc->bitDepth());
    PE_CHECK_EQ(snap->topLevelCount(), doc->topLevelCount());
    PE_CHECK_EQ(snap->activeLayer(), innerId);

    // Identity survives, including inside a nested group.
    const Layer* shotGroup = snap->findLayer(groupId);
    const Layer* shotInner = snap->findLayer(innerId);
    PE_CHECK(shotGroup != nullptr);
    PE_CHECK(shotInner != nullptr);
    if (shotInner == nullptr) return;
    PE_CHECK(shotInner != doc->findLayer(innerId));  // a copy, not the same object
    PE_CHECK(shotInner->name() == "Inner");
    PE_CHECK(shotInner->opacity() > 0.24f && shotInner->opacity() < 0.26f);
    PE_CHECK(static_cast<const PixelLayer*>(shotInner)->tiles().pixel(10, 10) ==
             Rgba8{5, 6, 7, 255});
}

PE_TEST(snapshot_carries_no_history_and_no_editing_session_state) {
    // Documented explicitly because a consumer that reads them would silently get nothing
    // rather than fail. Selection is the one that would bite: it stores its mask tiles by
    // value, so capturing it is a real copy and it is deliberately left out.
    auto doc = tiledDoc(2, 2);
    const LayerId id = doc->activeLayer();
    auto cmd = bucketFill(*doc, id, 4, 4, Rgbaf{0.0f, 1.0f, 0.0f, 1.0f}, 0, nullptr);
    PE_CHECK(cmd != nullptr);
    if (cmd == nullptr) return;
    doc->history().push(std::move(cmd));
    Selection sel;
    sel.selectRect(Rect{0, 0, kTileSize, kTileSize});
    doc->editableSelection() = std::move(sel);
    PE_CHECK(doc->selection().active());
    PE_CHECK(doc->isDirty());

    const auto snap = doc->snapshot();
    PE_CHECK(snap != nullptr);
    if (snap == nullptr) return;
    PE_CHECK(!snap->selection().active());
    PE_CHECK(!snap->isDirty());
    PE_CHECK(!snap->history().canUndo());
    PE_CHECK(!snap->history().canRedo());
}

PE_TEST(a_worker_reading_a_snapshot_is_isolated_from_concurrent_live_painting) {
    // The scenario the whole slice exists for: a worker holds a snapshot and reads it
    // while the owning thread keeps writing the live document. The worker checks every
    // byte it reads against what the snapshot held when it was taken, so a torn read, a
    // missed fork or an aliased buffer shows up as a mismatch rather than as a maybe.
    //
    // Synchronization here only opens the race window deliberately. There is no lock
    // between the reader and the writer: the isolation is supposed to come from ownership.
    constexpr int kCols = 4;
    constexpr int kRows = 3;
    constexpr int kWriteStride = 17;  // the reader checks these exact pixels
    auto doc = tiledDoc(kCols, kRows);
    const LayerId id = doc->activeLayer();
    const auto snap = doc->snapshot();
    PE_CHECK(snap != nullptr);
    if (snap == nullptr) return;

    std::atomic<bool> workerReady{false};
    std::atomic<bool> writerDone{false};
    std::atomic<int> mismatches{0};
    std::atomic<long long> reads{0};

    std::thread worker([&] {
        const TileStore& shot = storeOf(*snap, id);
        workerReady.store(true, std::memory_order_release);
        while (!writerDone.load(std::memory_order_acquire)) {
            for (int row = 0; row < kRows; ++row) {
                for (int col = 0; col < kCols; ++col) {
                    const TileStore::Tile* t = shot.find(TileCoord{col, row});
                    if (t == nullptr) {
                        ++mismatches;
                        continue;
                    }
                    const Rgba8 want = colorFor(col, row);
                    // Sample exactly the positions the writer below is writing to. A
                    // reader that sampled elsewhere would miss a missed fork almost
                    // every time, and the test would pass on a broken barrier.
                    for (int k = 0; k < kTileSize; k += kWriteStride) {
                        if (!(t->at(k, k) == want)) ++mismatches;
                    }
                    ++reads;
                }
            }
        }
    });

    while (!workerReady.load(std::memory_order_acquire)) {
    }

    // Keep painting. Each round re-snapshots, which marks every tile shared again, so the
    // writes that follow force a fresh fork per tile instead of settling down after the
    // first round.
    for (int round = 0; round < 40; ++round) {
        const auto churn = doc->snapshot();
        TileStore& live = mutableStoreOf(*doc, id);
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < kCols; ++col) {
                for (int k = 0; k < kTileSize; k += kWriteStride) {
                    live.setPixel(col * kTileSize + k, row * kTileSize + k,
                                  Rgba8{static_cast<std::uint8_t>(round), 1, 2, 255});
                }
            }
        }
    }
    writerDone.store(true, std::memory_order_release);
    worker.join();

    PE_CHECK_EQ(mismatches.load(), 0);
    PE_CHECK(reads.load() > 0);  // the worker really ran against a live writer

    // And the live document really did change out from under it.
    const TileStore& live = storeOf(*doc, id);
    PE_CHECK(live.find(TileCoord{0, 0})->at(kWriteStride, kWriteStride) == Rgba8{39, 1, 2, 255});
    PE_CHECK(storeOf(*snap, id).find(TileCoord{0, 0})->at(kWriteStride, kWriteStride) ==
             colorFor(0, 0));
}

PE_TEST(serializing_a_snapshot_gives_the_same_bytes_however_much_the_document_changes) {
    // What a save actually depends on, stated without any timing: the byte stream produced
    // from a snapshot is a function of the moment it was taken, and no later edit can move
    // it. The live document's own serialization must move, or this would pass on a
    // snapshot that had simply captured nothing.
    auto doc = tiledDoc(3, 2);
    const LayerId id = doc->activeLayer();
    const auto snap = doc->snapshot();
    PE_CHECK(snap != nullptr);
    if (snap == nullptr) return;

    const std::vector<std::byte> beforeEdits = exportDocument(*snap, ImageFormat::Native);
    PE_CHECK(!beforeEdits.empty());

    // Every kind of edit a save has to be insulated from: a committed pixel command, a raw
    // in-place write, and a change to the layer stack itself.
    auto cmd = bucketFill(*doc, id, 300, 300, Rgbaf{0.0f, 1.0f, 0.0f, 1.0f}, 0, nullptr);
    PE_CHECK(cmd != nullptr);
    if (cmd != nullptr) doc->history().push(std::move(cmd));
    mutableStoreOf(*doc, id).setPixel(7, 7, Rgba8{255, 255, 255, 255});
    auto extra = std::make_unique<PixelLayer>("Added during the save", BitDepth::U8);
    extra->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{1, 2, 3, 255});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(extra));

    const std::vector<std::byte> afterEdits = exportDocument(*snap, ImageFormat::Native);
    PE_CHECK(afterEdits == beforeEdits);  // the snapshot did not move

    const std::vector<std::byte> liveNow = exportDocument(*doc, ImageFormat::Native);
    PE_CHECK(!(liveNow == beforeEdits));  // and the live document did
}
