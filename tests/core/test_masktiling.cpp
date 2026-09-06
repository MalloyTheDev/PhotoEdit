// Masks that span more than one tile.
//
// Every other mask test in the suite runs on an 8x8 or 64x64 canvas, so every mask
// lives entirely in tile (0,0) and any confusion between a document coordinate and a
// tile-local one is invisible. These cover the two compositor mask loops on a canvas
// spanning a 3x3 grid of tiles, with a different constant per tile and two tiles left
// absent, so reading the wrong tile changes the result instead of hiding in it.

#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace pe;

namespace {

// Three tiles across and down, with the last one partial so the loops also see a tile
// the canvas does not fill.
constexpr int kW = 2 * kTileSize + 100;
constexpr int kH = 2 * kTileSize + 100;
const Rect kCanvas{0, 0, kW, kH};
constexpr Rgba8 kRed{255, 0, 0, 255};

// A distinct constant per tile. Tiles (1,1) and (2,2) are deliberately left out of the
// buffer entirely: absent reads as kOpaque, which is a different answer again.
constexpr uint8_t kAbsent = MaskBuffer::kOpaque;

uint8_t tileValue(int col, int row) {
    static constexpr uint8_t kByTile[3][3] = {
        {0, 64, 128},
        {192, kAbsent, 32},
        {96, 224, kAbsent},
    };
    return kByTile[row][col];
}

bool tileIsStored(int col, int row) {
    return tileValue(col, row) != kAbsent;
}

// The mask byte the buffer must report at a document pixel, derived from the tile grid
// rather than from MaskBuffer itself.
uint8_t expectedByte(int x, int y) {
    return tileValue(x / kTileSize, y / kTileSize);
}

std::unique_ptr<Mask> gridMask() {
    auto m = std::make_unique<Mask>();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (!tileIsStored(col, row)) continue;
            m->buffer().fillRect(Rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize},
                                 tileValue(col, row));
        }
    }
    return m;
}

// Pixels spread across all nine tiles, including tile-boundary neighbours where an
// off-by-one in the tile index would land in the wrong tile.
std::vector<Point> probePoints() {
    std::vector<Point> pts;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int bx = col * kTileSize;
            const int by = row * kTileSize;
            pts.push_back(Point{bx, by});                                   // tile origin
            pts.push_back(Point{bx + 37, by + 5});                          // interior
            if (col < 2) pts.push_back(Point{bx + kTileSize - 1, by + 3});  // last column
            if (row < 2) pts.push_back(Point{bx + 3, by + kTileSize - 1});  // last row
        }
    }
    std::vector<Point> inCanvas;  // drop anything past the partial final tile
    for (const Point& p : pts) {
        if (p.x < kW && p.y < kH) inCanvas.push_back(p);
    }
    return inCanvas;
}

int alphaAt(const PixelBuffer& img, Point p) {
    return static_cast<int>(img.at(p.x, p.y).a);
}
int redAt(const PixelBuffer& img, Point p) {
    return static_cast<int>(img.at(p.x, p.y).r);
}

PixelBuffer renderMasked(std::unique_ptr<Mask> mask) {
    auto layer = std::make_unique<SolidColorLayer>(kRed, kCanvas);
    layer->setMask(std::move(mask));
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(std::move(layer));
    return compositeToImage(stack, kCanvas);
}

// An opaque red backdrop with a masked Invert on top: where the mask reveals, the pixel
// goes cyan, so the red channel reads the mask coverage directly.
PixelBuffer renderAdjusted(std::unique_ptr<Mask> mask) {
    std::vector<std::unique_ptr<Layer>> stack;
    stack.push_back(std::make_unique<SolidColorLayer>(kRed, kCanvas));
    auto adj = std::make_unique<AdjustmentLayer>(std::make_unique<Invert>(), "Invert");
    adj->setMask(std::move(mask));
    stack.push_back(std::move(adj));
    return compositeToImage(stack, kCanvas);
}

}  // namespace

PE_TEST(maskbuffer_reports_the_right_tile_across_a_grid) {
    // The foundation the compositor tests below rest on: if this is wrong, they are
    // measuring the wrong thing.
    auto m = gridMask();
    for (const Point& p : probePoints()) {
        PE_CHECK_EQ(static_cast<int>(m->buffer().value(p.x, p.y)),
                    static_cast<int>(expectedByte(p.x, p.y)));
    }
    PE_CHECK_EQ(m->buffer().tileCount(), 7u);  // nine tiles less the two absent ones
}

PE_TEST(maskbuffer_negative_coordinates_stay_in_their_own_tile) {
    // Tile indexing floors toward negative infinity, so -1 belongs to tile -1 at local
    // offset 255, not to tile 0. Nothing in the compositor reaches negative document
    // coordinates, but Selection::toMask and the crop translate do.
    MaskBuffer b;
    b.setValue(-1, -1, 10);
    b.setValue(-kTileSize, -kTileSize, 20);  // origin of tile (-1,-1)
    b.setValue(0, 0, 30);                    // origin of tile (0,0)

    PE_CHECK_EQ(b.value(-1, -1), 10);
    PE_CHECK_EQ(b.value(-kTileSize, -kTileSize), 20);
    PE_CHECK_EQ(b.value(0, 0), 30);
    PE_CHECK_EQ(b.value(-2, -1), MaskBuffer::kOpaque);  // untouched neighbour, same tile
    PE_CHECK_EQ(b.tileCount(), 2u);
}

PE_TEST(compositor_layer_mask_follows_the_tile_grid) {
    // Coverage is the mask byte over 255 and the layer is fully opaque, so the
    // composited alpha comes back as the mask byte itself.
    const PixelBuffer img = renderMasked(gridMask());
    for (const Point& p : probePoints()) {
        const int want = static_cast<int>(expectedByte(p.x, p.y));
        PE_CHECK(std::abs(alphaAt(img, p) - want) <= 1);
    }
}

PE_TEST(compositor_layer_mask_inverted_follows_the_tile_grid) {
    auto m = gridMask();
    m->setInverted(true);
    const PixelBuffer img = renderMasked(std::move(m));
    for (const Point& p : probePoints()) {
        const int want = 255 - static_cast<int>(expectedByte(p.x, p.y));
        PE_CHECK(std::abs(alphaAt(img, p) - want) <= 1);
    }
}

PE_TEST(compositor_layer_mask_density_follows_the_tile_grid) {
    auto m = gridMask();
    m->setDensity(0.5f);
    const PixelBuffer img = renderMasked(std::move(m));
    for (const Point& p : probePoints()) {
        const int want = static_cast<int>(static_cast<float>(expectedByte(p.x, p.y)) * 0.5f);
        PE_CHECK(std::abs(alphaAt(img, p) - want) <= 1);
    }
}

PE_TEST(compositor_adjustment_mask_follows_the_tile_grid) {
    // The adjustment branch is a separate loop from the layer-mask one, and until now
    // exactly one test covered it: one tile, binary mask, density 1, not inverted.
    const PixelBuffer img = renderAdjusted(gridMask());
    for (const Point& p : probePoints()) {
        const int want = 255 - static_cast<int>(expectedByte(p.x, p.y));  // red lerps 255 -> 0
        PE_CHECK(std::abs(redAt(img, p) - want) <= 1);
        PE_CHECK_EQ(alphaAt(img, p), 255);  // an adjustment adds no coverage
    }
}

PE_TEST(compositor_adjustment_mask_inverted_and_density_follow_the_tile_grid) {
    // Neither inverted nor a partial density has ever been exercised on the adjustment
    // path, and that is exactly the arithmetic a hoist has to keep bit-identical.
    auto m = gridMask();
    m->setInverted(true);
    m->setDensity(0.5f);
    const PixelBuffer img = renderAdjusted(std::move(m));
    for (const Point& p : probePoints()) {
        const float cov = (1.0f - static_cast<float>(expectedByte(p.x, p.y)) / 255.0f) * 0.5f;
        const int want = static_cast<int>(255.0f - 255.0f * cov);
        PE_CHECK(std::abs(redAt(img, p) - want) <= 1);
    }
}
