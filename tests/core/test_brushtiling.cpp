// Brush strokes that cross tile boundaries.
//
// Every selection-gated and mask-painting test in the suite runs on a 16x16 or 64x64
// document, so the whole stroke lives in tile (0,0). Forcing every tile lookup in
// Brush.cpp to resolve tile (0,0) left the entire 512-case suite green, which means the
// brush engine had no coverage at all for reading the right tile. These cover the four
// per-pixel lookups the inner loops make: stroke coverage, the mask byte, the selection
// gate on the batched path, and the same gate on the incremental path.

#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using namespace pe;

namespace {

constexpr int kW = 2 * kTileSize + 100;  // a 3x3 grid of tiles, the last one partial
constexpr int kH = 2 * kTileSize + 100;
const Rect kCanvas{0, 0, kW, kH};
constexpr Rgba8 kGround{200, 200, 200, 255};
constexpr Rgbaf kInk{1.0f, 0.0f, 0.0f, 1.0f};

BrushSettings wideBrush(float diameter) {
    BrushSettings b;
    b.diameter = diameter;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    return b;
}

std::vector<StrokePoint> segment(float x0, float y0, float x1, float y1) {
    return {StrokePoint{Vec2{x0, y0}, 1.0f}, StrokePoint{Vec2{x1, y1}, 1.0f}};
}

std::unique_ptr<Document> groundDoc(LayerId& out) {
    auto doc = Document::createBlank(Size{kW, kH});
    out = doc->activeLayer();
    static_cast<PixelLayer*>(doc->findLayer(out))->tiles().fillRect(kCanvas, kGround);
    return doc;
}

Rgba8 pixelAt(const Document& doc, LayerId id, int x, int y) {
    return static_cast<const PixelLayer*>(doc.findLayer(id))->tiles().pixel(x, y);
}

bool isInked(const Document& doc, LayerId id, int x, int y) {
    const Rgba8 p = pixelAt(doc, id, x, y);
    return p.r > 200 && p.g < 60;
}

// A point well inside tile `index` along the canvas diagonal. The last tile is only
// 100 px of a 256 px tile, so its true centre would fall outside the canvas.
int tileMid(int index) {
    return index * kTileSize + (index == 2 ? 50 : kTileSize / 2);
}

}  // namespace

PE_TEST(brush_selection_gate_reads_the_pixel_own_tile) {
    // The selection covers exactly tile (1,1). A diagonal stroke crosses all three
    // diagonal tiles, so paint must appear in the middle one and nowhere else. Reading
    // any other tile's coverage either blocks the middle or lets a neighbour through.
    LayerId id = kNoLayer;
    auto doc = groundDoc(id);

    Selection sel;
    sel.selectRect(Rect{kTileSize, kTileSize, kTileSize, kTileSize});

    auto cmd = paintStroke(
        *doc, id, wideBrush(20.0f), kInk,
        segment(40.0f, 40.0f, static_cast<float>(kW - 40), static_cast<float>(kH - 40)), &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK(isInked(*doc, id, tileMid(1), tileMid(1)));   // selected tile: painted
    PE_CHECK(!isInked(*doc, id, tileMid(0), tileMid(0)));  // tile (0,0): blocked
    PE_CHECK(!isInked(*doc, id, tileMid(2), tileMid(2)));  // tile (2,2): blocked
    PE_CHECK_EQ(pixelAt(*doc, id, tileMid(0), tileMid(0)), kGround);
    PE_CHECK_EQ(pixelAt(*doc, id, tileMid(2), tileMid(2)), kGround);

    // Right at the selected tile's edges, where an off-by-one tile index would show.
    PE_CHECK(!isInked(*doc, id, kTileSize - 2, kTileSize - 2));
    PE_CHECK(!isInked(*doc, id, 2 * kTileSize + 1, 2 * kTileSize + 1));
}

PE_TEST(livestroke_selection_gate_reads_the_pixel_own_tile) {
    // The incremental path has its own copy of the gate, hoisted separately.
    LayerId id = kNoLayer;
    auto doc = groundDoc(id);

    Selection sel;
    sel.selectRect(Rect{kTileSize, kTileSize, kTileSize, kTileSize});

    auto live = beginPaintStroke(*doc, id, wideBrush(20.0f), kInk, &sel);
    PE_CHECK(live != nullptr);
    std::vector<StrokePoint> acc;
    for (int i = 0; i <= 12; ++i) {
        const float t = static_cast<float>(i) / 12.0f;
        acc.push_back(StrokePoint{
            Vec2{40.0f + t * static_cast<float>(kW - 80), 40.0f + t * static_cast<float>(kH - 80)},
            1.0f});
        (void)live->extend(acc);
    }
    auto cmd = live->finish();
    PE_CHECK(cmd != nullptr);

    PE_CHECK(isInked(*doc, id, tileMid(1), tileMid(1)));
    PE_CHECK(!isInked(*doc, id, tileMid(0), tileMid(0)));
    PE_CHECK(!isInked(*doc, id, tileMid(2), tileMid(2)));
}

PE_TEST(brush_coverage_is_found_in_a_tile_far_from_the_origin) {
    // The region-bake brushes look the stroke's coverage up per pixel. A stroke confined
    // to the far tile has no coverage anywhere near the origin, so resolving the wrong
    // tile yields zero coverage and the bake becomes a no-op.
    LayerId id = kNoLayer;
    auto doc = groundDoc(id);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(id));
    // A hard edge inside tile (2,2) for the blur to soften.
    pl->tiles().fillRect(Rect{2 * kTileSize, 2 * kTileSize, 50, 100}, Rgba8{0, 0, 0, 255});

    const int ex = 2 * kTileSize + 50;  // the edge column
    const int ey = 2 * kTileSize + 50;
    PE_CHECK_EQ(pixelAt(*doc, id, ex - 1, ey), (Rgba8{0, 0, 0, 255}));
    PE_CHECK_EQ(pixelAt(*doc, id, ex, ey), kGround);

    auto cmd = blurStroke(*doc, id, wideBrush(30.0f),
                          segment(static_cast<float>(ex), static_cast<float>(ey - 20),
                                  static_cast<float>(ex), static_cast<float>(ey + 20)),
                          nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    // The edge is no longer a step: both sides moved toward each other.
    const Rgba8 dark = pixelAt(*doc, id, ex - 1, ey);
    const Rgba8 light = pixelAt(*doc, id, ex, ey);
    PE_CHECK(dark.r > 0);
    PE_CHECK(light.r < 200);
}

PE_TEST(maskpaint_reads_and_restores_each_tile_own_bytes) {
    // maskPaintStroke snapshots the mask byte per pixel to build its undo record. Give
    // every tile a distinct starting value and stroke across all of them: if the read
    // resolves the wrong tile, undo writes one tile's value back over the others.
    LayerId id = kNoLayer;
    auto doc = groundDoc(id);
    Layer* layer = doc->findLayer(id);
    auto mask = std::make_unique<Mask>();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const auto v = static_cast<std::uint8_t>(20 + 30 * (row * 3 + col));
            mask->buffer().fillRect(Rect{col * kTileSize, row * kTileSize, kTileSize, kTileSize},
                                    v);
        }
    }
    layer->setMask(std::move(mask));

    const MaskBuffer& buf = doc->findLayer(id)->mask()->buffer();
    std::vector<std::uint8_t> original;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) original.push_back(buf.value(tileMid(col), tileMid(row)));
    }

    auto cmd = maskPaintStroke(
        *doc, id, wideBrush(40.0f),
        segment(40.0f, 40.0f, static_cast<float>(kW - 40), static_cast<float>(kH - 40)), 0.0f,
        nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    // The stroke really crossed the far tile, not just the first one.
    PE_CHECK(buf.value(tileMid(2), tileMid(2)) < original[8]);
    PE_CHECK(buf.value(tileMid(0), tileMid(0)) < original[0]);

    doc->history().undo();
    std::size_t k = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col, ++k) {
            PE_CHECK_EQ(static_cast<int>(buf.value(tileMid(col), tileMid(row))),
                        static_cast<int>(original[k]));
        }
    }
}

PE_TEST(maskpaint_selection_gate_reads_the_pixel_own_tile) {
    LayerId id = kNoLayer;
    auto doc = groundDoc(id);
    doc->findLayer(id)->setMask(std::make_unique<Mask>());

    Selection sel;
    sel.selectRect(Rect{kTileSize, kTileSize, kTileSize, kTileSize});

    auto cmd = maskPaintStroke(
        *doc, id, wideBrush(20.0f),
        segment(40.0f, 40.0f, static_cast<float>(kW - 40), static_cast<float>(kH - 40)), 0.0f,
        &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    const MaskBuffer& buf = doc->findLayer(id)->mask()->buffer();
    PE_CHECK(buf.value(tileMid(1), tileMid(1)) < MaskBuffer::kOpaque);    // selected: painted
    PE_CHECK_EQ(buf.value(tileMid(0), tileMid(0)), MaskBuffer::kOpaque);  // blocked
    PE_CHECK_EQ(buf.value(tileMid(2), tileMid(2)), MaskBuffer::kOpaque);  // blocked
}

PE_TEST(dab_on_a_tile_corner_covers_all_four_tiles) {
    // stampDab memoizes the coverage tile across its scan. A dab centred on the corner
    // where four tiles meet has to switch tiles several times per row, which is the case
    // a memo gets wrong.
    LayerId id = kNoLayer;
    auto doc = groundDoc(id);

    const auto c = static_cast<float>(kTileSize);
    auto cmd = paintStroke(*doc, id, wideBrush(60.0f), kInk, segment(c, c, c, c), nullptr);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK(isInked(*doc, id, kTileSize - 10, kTileSize - 10));  // tile (0,0)
    PE_CHECK(isInked(*doc, id, kTileSize + 10, kTileSize - 10));  // tile (1,0)
    PE_CHECK(isInked(*doc, id, kTileSize - 10, kTileSize + 10));  // tile (0,1)
    PE_CHECK(isInked(*doc, id, kTileSize + 10, kTileSize + 10));  // tile (1,1)
    PE_CHECK(!isInked(*doc, id, kTileSize - 60, kTileSize));      // outside the dab
}
