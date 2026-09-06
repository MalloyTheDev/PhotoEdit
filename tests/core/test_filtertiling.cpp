// Destructive pixel edits over a region that spans more than one tile.
//
// Every selection-gated test in the suite runs on a 16x16, 32x32 or 64x64 document, so
// the whole edit lives in tile (0,0) and a confusion between a document coordinate and
// a tile-local one cannot show up. These run bakePixelEditRegion over a 3x3 grid of
// tiles with a selection shaped so that each tile gets a different answer, which is what
// makes a wrong tile index visible.

#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace pe;

namespace {

constexpr int kW = 2 * kTileSize + 100;  // three tiles across, the last one partial
constexpr int kH = 2 * kTileSize + 100;
const Rect kCanvas{0, 0, kW, kH};
constexpr Rgba8 kBefore{40, 80, 120, 255};
constexpr Rgba8 kAfter{255, 0, 0, 255};

// Paint every pixel of the extracted region red, so "was this pixel edited" is a plain
// colour comparison rather than a tolerance question.
void paintRed(std::span<Rgbaf> img, int, int) {
    for (Rgbaf& p : img) p = Rgbaf{1.0f, 0.0f, 0.0f, 1.0f};
}

struct Fixture {
    std::unique_ptr<Document> doc;
    LayerId layer = kNoLayer;
    PixelLayer* pl = nullptr;
};

Fixture makeFixture() {
    Fixture f;
    f.doc = Document::createBlank(Size{kW, kH});
    f.layer = f.doc->activeLayer();
    f.pl = static_cast<PixelLayer*>(f.doc->findLayer(f.layer));
    f.pl->tiles().fillRect(kCanvas, kBefore);
    return f;
}

bool isEdited(const Fixture& f, int x, int y) {
    return f.pl->tiles().pixel(x, y) == kAfter;
}
bool isUntouched(const Fixture& f, int x, int y) {
    return f.pl->tiles().pixel(x, y) == kBefore;
}

}  // namespace

PE_TEST(bake_selection_gate_follows_the_tile_grid) {
    // The selection covers exactly tile (1,1), the middle of a 3x3 grid. If the gate
    // resolves the wrong tile, the edit lands in a neighbour and both halves of this
    // test fail rather than one.
    Fixture f = makeFixture();
    Selection sel;
    sel.selectRect(Rect{kTileSize, kTileSize, kTileSize, kTileSize});

    auto cmd = bakePixelEditRegion(*f.doc, f.layer, "Paint", kCanvas, paintRed, &sel);
    PE_CHECK(cmd != nullptr);
    f.doc->history().push(std::move(cmd));

    // Inside the selected tile, including all four of its corners.
    PE_CHECK(isEdited(f, kTileSize, kTileSize));
    PE_CHECK(isEdited(f, 2 * kTileSize - 1, kTileSize));
    PE_CHECK(isEdited(f, kTileSize, 2 * kTileSize - 1));
    PE_CHECK(isEdited(f, 2 * kTileSize - 1, 2 * kTileSize - 1));
    PE_CHECK(isEdited(f, kTileSize + 130, kTileSize + 77));

    // Every one of the eight neighbouring tiles, sampled just across the boundary.
    PE_CHECK(isUntouched(f, kTileSize - 1, kTileSize));      // west
    PE_CHECK(isUntouched(f, 2 * kTileSize, kTileSize));      // east
    PE_CHECK(isUntouched(f, kTileSize, kTileSize - 1));      // north
    PE_CHECK(isUntouched(f, kTileSize, 2 * kTileSize));      // south
    PE_CHECK(isUntouched(f, kTileSize - 1, kTileSize - 1));  // northwest
    PE_CHECK(isUntouched(f, 2 * kTileSize, kTileSize - 1));  // northeast
    PE_CHECK(isUntouched(f, kTileSize - 1, 2 * kTileSize));  // southwest
    PE_CHECK(isUntouched(f, 2 * kTileSize, 2 * kTileSize));  // southeast
    PE_CHECK(isUntouched(f, 0, 0));
    PE_CHECK(isUntouched(f, kW - 1, kH - 1));
}

PE_TEST(bake_selection_gate_splits_a_tile_boundary) {
    // A selection rectangle straddling the corner where four tiles meet: each of the
    // four sees a different quadrant of it, so no single tile's answer is reusable.
    Fixture f = makeFixture();
    Selection sel;
    sel.selectRect(Rect{kTileSize - 60, kTileSize - 60, 120, 120});

    auto cmd = bakePixelEditRegion(*f.doc, f.layer, "Paint", kCanvas, paintRed, &sel);
    PE_CHECK(cmd != nullptr);
    f.doc->history().push(std::move(cmd));

    PE_CHECK(isEdited(f, kTileSize - 1, kTileSize - 1));  // inside, tile (0,0)
    PE_CHECK(isEdited(f, kTileSize, kTileSize - 1));      // inside, tile (1,0)
    PE_CHECK(isEdited(f, kTileSize - 1, kTileSize));      // inside, tile (0,1)
    PE_CHECK(isEdited(f, kTileSize, kTileSize));          // inside, tile (1,1)

    PE_CHECK(isUntouched(f, kTileSize - 61, kTileSize));  // one pixel left of the rect
    PE_CHECK(isUntouched(f, kTileSize + 60, kTileSize));  // one pixel right of it
    PE_CHECK(isUntouched(f, kTileSize, kTileSize - 61));  // one pixel above
    PE_CHECK(isUntouched(f, kTileSize, kTileSize + 60));  // one pixel below
}

PE_TEST(bake_reads_the_right_source_tile_across_a_grid) {
    // The extraction that feeds the transform is a separate loop from the writeback.
    // Give every tile a distinct colour and have the transform copy its input through
    // unchanged: anything that reads the wrong source tile smears one tile's colour
    // into another.
    Fixture f = makeFixture();
    auto colorFor = [](int col, int row) {
        return Rgba8{static_cast<uint8_t>(10 + 30 * col), static_cast<uint8_t>(10 + 30 * row), 200,
                     255};
    };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            f.pl->tiles().fillRect(Rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize},
                                   colorFor(col, row));
        }
    }

    // A no-op transform still round-trips every pixel through extract and writeback.
    auto cmd = bakePixelEditRegion(
        *f.doc, f.layer, "Identity", kCanvas, [](std::span<Rgbaf>, int, int) {}, nullptr);
    if (cmd != nullptr) f.doc->history().push(std::move(cmd));  // a true no-op may return null

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int x = col * kTileSize + 7;
            const int y = row * kTileSize + 11;
            if (x >= kW || y >= kH) continue;
            PE_CHECK_EQ(f.pl->tiles().pixel(x, y), colorFor(col, row));
        }
    }
}

PE_TEST(bake_partial_selection_coverage_is_per_tile) {
    // Feathering pushes coverage off the 0/255 extremes, so the gate lerps rather than
    // switching. Reading a neighbouring tile's coverage would land on the wrong ratio
    // instead of merely on the wrong side of a threshold.
    Fixture f = makeFixture();
    Selection sel;
    sel.selectRect(Rect{kTileSize, kTileSize, kTileSize, kTileSize});
    sel.feather(20.0f, kCanvas);

    auto cmd = bakePixelEditRegion(*f.doc, f.layer, "Paint", kCanvas, paintRed, &sel);
    PE_CHECK(cmd != nullptr);
    f.doc->history().push(std::move(cmd));

    // Deep inside the feathered tile the edit is at full strength.
    const Rgba8 core = f.pl->tiles().pixel(kTileSize + 128, kTileSize + 128);
    PE_CHECK_EQ(core, kAfter);
    // Well outside it, nothing moved at all.
    PE_CHECK(isUntouched(f, 0, 0));
    PE_CHECK(isUntouched(f, kW - 1, kH - 1));
    // On the feathered edge the pixel sits strictly between the two, which only holds
    // if the coverage came from this pixel's own tile.
    const Rgba8 edge = f.pl->tiles().pixel(kTileSize + 2, kTileSize + 128);
    PE_CHECK(edge.r > kBefore.r && edge.r < kAfter.r);
    PE_CHECK(edge.g < kBefore.g && edge.g > kAfter.g);
}
