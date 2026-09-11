// resampleLayerContent: the tile-streamed, depth-generic per-layer resampler behind Image Size.
// Its correctness anchor is the contiguous resampleImage from test_resample: the tiled path,
// sampling the source CANVAS, must produce the same pixels as the oracle over the destination
// canvas. The rest pins the command contract (undo, the vacated clear, budget, depth).

#include "pe/core/Color.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Resample.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pe;

namespace {

// A canvas-filling gradient, so the resample has real detail to move.
std::unique_ptr<Document> gradientDoc(int w, int h) {
    auto doc = Document::createBlank(Size{w, h});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            pl->tiles().setPixel(x, y,
                                 Rgba8{static_cast<uint8_t>(x * 255 / (w - 1)),
                                       static_cast<uint8_t>(y * 255 / (h - 1)),
                                       static_cast<uint8_t>((x + y) * 255 / (w + h - 2)), 255});
        }
    }
    return doc;
}

const PixelLayer* pixels(const Document& doc, LayerId id) {
    return static_cast<const PixelLayer*>(doc.findLayer(id));
}

std::vector<Rgbaf> readCanvas(const Document& doc, LayerId id, int w, int h) {
    const auto* pl = pixels(doc, id);
    std::vector<Rgbaf> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            out[static_cast<std::size_t>(y) * w + x] = toFloat(pl->tiles().pixel(x, y));
        }
    }
    return out;
}

}  // namespace

PE_TEST(resamplelayer_tiled_path_matches_the_contiguous_oracle) {
    // The load-bearing test. A canvas-filling layer resampled by the tile-streamed command must
    // agree pixel for pixel (bar 8-bit rounding) with the same source run through the contiguous
    // resampleImage over the source canvas -- or the tiling/banding is wrong.
    for (const auto [dw, dh] : {std::pair{320, 200}, std::pair{100, 100}, std::pair{500, 300}}) {
        auto doc = gradientDoc(200, 150);
        const LayerId id = doc->activeLayer();
        const std::vector<Rgbaf> srcBuf = readCanvas(*doc, id, 200, 150);
        const std::vector<Rgbaf> oracle = resampleImage(srcBuf, 200, 150, dw, dh);
        PE_REQUIRE(!oracle.empty());

        auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 200, 150}, Rect{0, 0, dw, dh});
        PE_REQUIRE(cmd != nullptr);
        doc->history().push(std::move(cmd));

        int mismatches = 0;
        int maxDiff = 0;
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const Rgba8 got = pixels(*doc, id)->tiles().pixel(x, y);
                const Rgba8 want = fromFloat<Rgba8>(oracle[static_cast<std::size_t>(y) * dw + x]);
                const int d =
                    std::max(std::max(std::abs(got.r - want.r), std::abs(got.g - want.g)),
                             std::max(std::abs(got.b - want.b), std::abs(got.a - want.a)));
                if (d > 1) ++mismatches;
                maxDiff = std::max(maxDiff, d);
            }
        }
        if (mismatches != 0) {
            std::printf("      %dx%d: %d mismatches, max channel diff %d\n", dw, dh, mismatches,
                        maxDiff);
        }
        PE_CHECK_EQ(mismatches, 0);
    }
}

PE_TEST(resamplelayer_upscale_and_undo_round_trips_exactly) {
    auto doc = gradientDoc(64, 64);
    const LayerId id = doc->activeLayer();
    const Rgba8 a = pixels(*doc, id)->tiles().pixel(10, 10);
    const Rgba8 b = pixels(*doc, id)->tiles().pixel(63, 0);
    const Rgba8 c = pixels(*doc, id)->tiles().pixel(32, 40);

    auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 64, 64}, Rect{0, 0, 128, 128});
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));
    // Content doubled: a pixel near (64,80) exists and is opaque where the source had content.
    PE_CHECK_EQ(pixels(*doc, id)->tiles().pixel(64, 80).a, static_cast<uint8_t>(255));

    doc->history().undo();
    PE_CHECK(pixels(*doc, id)->tiles().pixel(10, 10) == a);
    PE_CHECK(pixels(*doc, id)->tiles().pixel(63, 0) == b);
    PE_CHECK(pixels(*doc, id)->tiles().pixel(32, 40) == c);
}

PE_TEST(resamplelayer_downscale_clears_the_vacated_source) {
    // Shrinking to a quarter must leave the rest transparent, not a ghost of the old pixels.
    auto doc = gradientDoc(128, 128);
    const LayerId id = doc->activeLayer();
    PE_REQUIRE(pixels(*doc, id)->tiles().pixel(100, 100).a == 255);

    auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 128, 128}, Rect{0, 0, 32, 32});
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK_EQ(pixels(*doc, id)->tiles().pixel(10, 10).a, static_cast<uint8_t>(255));  // here now
    PE_CHECK_EQ(pixels(*doc, id)->tiles().pixel(100, 100).a, static_cast<uint8_t>(0));  // cleared
}

PE_TEST(resamplelayer_identity_scale_changes_nothing) {
    auto doc = gradientDoc(64, 48);
    const LayerId id = doc->activeLayer();
    auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 64, 48}, Rect{0, 0, 64, 48});
    PE_CHECK(cmd == nullptr);  // exact, no delta, no command
}

PE_TEST(resamplelayer_preserves_bit_depth) {
    auto doc = Document::createBlank(Size{64, 64}, ColorMode::RGB, BitDepth::U16);
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            pl->tiles16().setPixel(x, y,
                                   Rgba16{static_cast<uint16_t>(x * 1000),
                                          static_cast<uint16_t>(y * 1000), 40000, 65535});
        }
    }
    const LayerId id = doc->activeLayer();
    auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 64, 64}, Rect{0, 0, 128, 128});
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(pixels(*doc, id)->depth() == BitDepth::U16);
    const Rgba16 p = pixels(*doc, id)->tiles16().pixel(2, 2);
    PE_CHECK(p.b > 39000 && p.b <= 65535);  // 16-bit precision survives (not quantised via 8-bit)
}

PE_TEST(resamplelayer_premultiplies_so_transparent_colour_does_not_bleed) {
    // The opaque fixtures elsewhere make premultiply a no-op (transparent is {0,0,0,0}). Here
    // the transparent half stores a real magenta with alpha 0, which straight-alpha resampling
    // would smear into the green half as a red fringe. Premultiplied, it cannot.
    auto doc = Document::createBlank(Size{64, 64});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            pl->tiles().setPixel(x, y, x < 32 ? Rgba8{0, 255, 0, 255} : Rgba8{255, 0, 255, 0});
        }
    }
    const LayerId id = doc->activeLayer();
    auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 64, 64}, Rect{0, 0, 128, 128});
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));
    // Wherever the green shows through (alpha >= 100), red stays low. Straight-alpha bleed drives
    // the boundary red up toward 255 as the magenta mixes in; a >= g comparison misses it because
    // green stays high too, so pin the red directly.
    const auto* out = pixels(*doc, id);
    int maxRedOnGreenSide = 0;
    for (int y = 20; y < 108; ++y) {
        for (int x = 0; x < 128; ++x) {
            const Rgba8 q = out->tiles().pixel(x, y);
            if (q.a < 100) continue;
            maxRedOnGreenSide = std::max(maxRedOnGreenSide, static_cast<int>(q.r));
        }
    }
    // Premultiplied, the green side reads red 0; straight-alpha bleed measured 65 here. The
    // threshold sits between so the test cleanly separates the two.
    PE_CHECK(maxRedOnGreenSide < 20);
}

PE_TEST(resamplelayer_refuses_an_upscale_over_the_byte_budget) {
    // A resample is charged in bytes by the tiles it touches, like a Move. An upscale whose
    // destination blows kMaxMoveBytes is refused (nullptr) rather than attempted.
    auto doc = gradientDoc(64, 64);
    const LayerId id = doc->activeLayer();
    // 40000x40000 8-bit is ~24 000 tiles x 256 KiB ~ 6 GiB, well over the 1 GiB per-op budget,
    // and within the per-side coordinate limit so only the budget can stop it.
    PE_CHECK(resampleLayerContent(*doc, id, Rect{0, 0, 64, 64}, Rect{0, 0, 40000, 40000}) ==
             nullptr);
}

PE_TEST(resamplelayer_refuses_what_it_cannot_hold_or_scale) {
    auto doc = gradientDoc(32, 32);
    const LayerId id = doc->activeLayer();
    PE_CHECK(resampleLayerContent(*doc, id, Rect{}, Rect{0, 0, 8, 8}) == nullptr);    // empty src
    PE_CHECK(resampleLayerContent(*doc, id, Rect{0, 0, 32, 32}, Rect{}) == nullptr);  // empty dst
    PE_CHECK(resampleLayerContent(*doc, kNoLayer, Rect{0, 0, 32, 32}, Rect{0, 0, 8, 8}) == nullptr);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<PixelLayer>("Empty"));
    const LayerId empty = doc->topLevelLayers().back()->id();
    PE_CHECK(resampleLayerContent(*doc, empty, Rect{0, 0, 32, 32}, Rect{0, 0, 8, 8}) == nullptr);
}

PE_TEST(resamplelayer_a_flat_fill_stays_flat_and_opaque) {
    // No ringing or edge fade on a canvas-filling uniform layer: clamp-to-edge to the canvas
    // means the border resamples to the same colour, and premultiply keeps alpha solid.
    auto doc = Document::createBlank(Size{50, 50});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 50, 50}, Rgba8{40, 160, 210, 255});
    const LayerId id = doc->activeLayer();
    auto cmd = resampleLayerContent(*doc, id, Rect{0, 0, 50, 50}, Rect{0, 0, 155, 35});
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));

    const auto* out = pixels(*doc, id);
    for (int y = 0; y < 35; y += 5) {
        for (int x = 0; x < 155; x += 5) {
            const Rgba8 p = out->tiles().pixel(x, y);
            PE_CHECK(std::abs(p.r - 40) <= 2);
            PE_CHECK(std::abs(p.g - 160) <= 2);
            PE_CHECK(std::abs(p.b - 210) <= 2);
            PE_CHECK_EQ(p.a, static_cast<uint8_t>(255));  // opaque to the very edge, no rim
        }
    }
}
