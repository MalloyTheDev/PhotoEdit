#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/TileStore.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

using namespace pe;

namespace {
constexpr int kW = 64;
constexpr int kH = 64;

BrushSettings brush(float diameter, BlendMode blend = BlendMode::Normal) {
    BrushSettings b;
    b.diameter = diameter;
    b.hardness = 0.8f;
    b.opacity = 0.9f;
    b.flow = 0.7f;
    b.spacing = 0.25f;
    b.blendMode = blend;
    return b;
}

// A doc whose base pixel layer holds a gradient (so dodge/burn/clone have real content to work on).
std::unique_ptr<Document> gradientDoc(LayerId& outLayer) {
    auto doc = Document::createBlank(Size{kW, kH});
    outLayer = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(outLayer));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            pl->tiles().setPixel(x, y,
                                 Rgba8{static_cast<std::uint8_t>(x * 3 % 256),
                                       static_cast<std::uint8_t>(y * 3 % 256), 128, 255});
        }
    }
    return doc;
}

// A bent multi-point stroke (enough samples that the incremental/batched stepping is exercised).
std::vector<StrokePoint> path() {
    std::vector<StrokePoint> p;
    for (int i = 0; i <= 40; ++i) {
        const float t = static_cast<float>(i);
        p.push_back({{8.0f + t * 1.2f, 20.0f + 12.0f * std::sin(t * 0.3f)}, 1.0f});
    }
    return p;
}

std::vector<std::uint8_t> composite(Document& doc) {
    const PixelBuffer img = doc.compositeImage();
    const auto* d = reinterpret_cast<const std::uint8_t*>(img.data());
    return std::vector<std::uint8_t>(d,
                                     d + static_cast<std::size_t>(img.width()) * img.height() * 4);
}

// Apply the BATCHED stroke command and return the resulting composite.
using BatchFn = std::function<std::unique_ptr<PaintCommand>(
    Document&, LayerId, const BrushSettings&, std::span<const StrokePoint>, const Selection*)>;
using LiveFn = std::function<std::unique_ptr<LiveStroke>(Document&, LayerId, const BrushSettings&,
                                                         const Selection*)>;

// Core check: the incremental LiveStroke (fed in `chunk`-sized steps) must produce a composite
// byte-identical to the batched stroke, and its committed command must undo/redo byte-exactly.
void checkParity(const char* label, const BatchFn& batch, const LiveFn& live,
                 const BrushSettings& b, const Selection* sel, int chunk) {
    (void)label;
    const std::vector<StrokePoint> pts = path();

    // Batched reference.
    LayerId la = kNoLayer;
    auto da = gradientDoc(la);
    auto cmdA = batch(*da, la, b, pts, sel);
    PE_CHECK(cmdA != nullptr);
    cmdA->execute(*da);
    const std::vector<std::uint8_t> refImg = composite(*da);

    // Incremental, fed in chunks.
    LayerId lb = kNoLayer;
    auto db = gradientDoc(lb);
    const std::vector<std::uint8_t> initImg = composite(*db);
    auto stroke = live(*db, lb, b, sel);
    PE_CHECK(stroke != nullptr);
    std::vector<StrokePoint> acc;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        acc.push_back(pts[i]);
        if (static_cast<int>(acc.size()) % chunk == 0 || i + 1 == pts.size()) {
            (void)stroke->extend(acc);
        }
    }
    auto cmdB = stroke->finish();
    PE_CHECK(cmdB != nullptr);
    const std::vector<std::uint8_t> liveImg = composite(*db);

    PE_CHECK(liveImg == refImg);  // incremental == batched, byte for byte

    // The committed command is a byte-exact single undo step (store already holds the final pixels,
    // so pushing re-applies a no-op; undo restores the initial; redo reproduces the result).
    db->history().push(std::move(cmdB));
    PE_CHECK(composite(*db) == refImg);
    db->history().undo();
    PE_CHECK(composite(*db) == initImg);
    db->history().redo();
    PE_CHECK(composite(*db) == refImg);
}

// Read a layer's NATIVE store as a flat float vector. compositeImage() flattens to 8-bit and clamps
// to [0,1], which hides HDR/super-white divergence on the no-RGB-clamp clone/dodge paths — so depth
// parity must compare the native store, not the composite.
std::vector<float> sampleF32(Document& doc, LayerId l) {
    auto* pl = static_cast<PixelLayer*>(doc.findLayer(l));
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgbaf p = pl->tilesF().pixel(x, y);
            v.insert(v.end(), {p.r, p.g, p.b, p.a});
        }
    }
    return v;
}

std::vector<float> sampleU16(Document& doc, LayerId l) {
    auto* pl = static_cast<PixelLayer*>(doc.findLayer(l));
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba16 p = pl->tiles16().pixel(x, y);
            v.insert(v.end(), {static_cast<float>(p.r), static_cast<float>(p.g),
                               static_cast<float>(p.b), static_cast<float>(p.a)});
        }
    }
    return v;
}

using BuildFn = std::function<std::unique_ptr<Document>(LayerId&)>;
using SampleFn = std::function<std::vector<float>(Document&, LayerId)>;

// Depth parity: drive the live begin*/extend/finish path on a NON-U8 layer and compare the native
// store, byte-for-byte, to the batched stroke. Covers the LiveStrokeImpl<Rgba16>/<Rgbaf>
// instantiations and the beginLive() depth dispatch, which the U8 tests never reach.
void checkDepthParity(const BuildFn& build, const BatchFn& batch, const LiveFn& live,
                      const SampleFn& sample, const BrushSettings& b, int chunk) {
    const std::vector<StrokePoint> pts = path();

    LayerId la = kNoLayer;
    auto da = build(la);
    auto cmdA = batch(*da, la, b, pts, nullptr);
    PE_CHECK(cmdA != nullptr);
    cmdA->execute(*da);
    const std::vector<float> ref = sample(*da, la);

    LayerId lb = kNoLayer;
    auto db = build(lb);
    const std::vector<float> init = sample(*db, lb);
    auto stroke = live(*db, lb, b, nullptr);
    PE_CHECK(stroke != nullptr);
    std::vector<StrokePoint> acc;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        acc.push_back(pts[i]);
        if (static_cast<int>(acc.size()) % chunk == 0 || i + 1 == pts.size()) {
            (void)stroke->extend(acc);
        }
    }
    auto cmdB = stroke->finish();
    PE_CHECK(cmdB != nullptr);
    PE_CHECK(sample(*db, lb) == ref);  // incremental == batched in the native depth

    db->history().push(std::move(cmdB));
    PE_CHECK(sample(*db, lb) == ref);
    db->history().undo();
    PE_CHECK(sample(*db, lb) == init);
    db->history().redo();
    PE_CHECK(sample(*db, lb) == ref);
}

// An F32 doc whose content includes a super-white (>1.0) channel, so clone/dodge exercise the
// no-RGB-clamp HDR paths (a U8 layer would clamp and mask any divergence).
std::unique_ptr<Document> hdrDocF32(LayerId& outLayer) {
    auto doc = Document::createBlank(Size{kW, kH}, ColorMode::RGB, BitDepth::F32);
    outLayer = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(outLayer));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            pl->tilesF().setPixel(
                x, y,
                Rgbaf{0.1f + 0.01f * static_cast<float>(x), 1.5f,
                      0.4f + 0.008f * static_cast<float>(y), 1.0f});  // g is super-white
        }
    }
    return doc;
}

std::unique_ptr<Document> gradientDocU16(LayerId& outLayer) {
    auto doc = Document::createBlank(Size{kW, kH}, ColorMode::RGB, BitDepth::U16);
    outLayer = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(outLayer));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            pl->tiles16().setPixel(
                x, y,
                Rgba16{static_cast<std::uint16_t>(x * 1000 % 65536),
                       static_cast<std::uint16_t>(y * 1000 % 65536), 32768, 65535});
        }
    }
    return doc;
}
}  // namespace

PE_TEST(livestroke_paint_matches_batched) {
    const BrushSettings b = brush(14);
    checkParity(
        "paint",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) {
            return paintStroke(d, l, s, Rgbaf{0.9f, 0.1f, 0.2f, 1.0f}, p, sel);
        },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginPaintStroke(d, l, s, Rgbaf{0.9f, 0.1f, 0.2f, 1.0f}, sel);
        },
        b, nullptr, /*chunk=*/1);
}

PE_TEST(livestroke_paint_multiply_blend_matches_batched) {
    const BrushSettings b = brush(14, BlendMode::Multiply);
    checkParity(
        "paint-multiply",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) {
            return paintStroke(d, l, s, Rgbaf{0.3f, 0.6f, 0.9f, 1.0f}, p, sel);
        },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginPaintStroke(d, l, s, Rgbaf{0.3f, 0.6f, 0.9f, 1.0f}, sel);
        },
        b, nullptr, /*chunk=*/3);
}

PE_TEST(livestroke_erase_matches_batched) {
    const BrushSettings b = brush(18);
    checkParity(
        "erase",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) { return eraseStroke(d, l, s, p, sel); },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginEraseStroke(d, l, s, sel);
        },
        b, nullptr, /*chunk=*/1);
}

PE_TEST(livestroke_dodge_matches_batched) {
    const BrushSettings b = brush(16);
    checkParity(
        "dodge",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) { return dodgeStroke(d, l, s, p, sel); },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginDodgeStroke(d, l, s, sel);
        },
        b, nullptr, /*chunk=*/2);
}

PE_TEST(livestroke_burn_matches_batched) {
    const BrushSettings b = brush(16);
    checkParity(
        "burn",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) { return burnStroke(d, l, s, p, sel); },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginBurnStroke(d, l, s, sel);
        },
        b, nullptr, /*chunk=*/5);
}

PE_TEST(livestroke_clone_matches_batched_no_feedback) {
    // Clone reads the PRE-STROKE source even where source/dest overlap; the incremental path must
    // match (it samples the per-tile snapshot, not the live store).
    const BrushSettings b = brush(16);
    const int offX = 12;  // source = dest - offset
    const int offY = 8;
    checkParity(
        "clone",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) { return cloneStroke(d, l, s, p, offX, offY, sel); },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginCloneStroke(d, l, s, offX, offY, sel);
        },
        b, nullptr, /*chunk=*/1);
}

PE_TEST(livestroke_selection_gated_matches_batched) {
    const BrushSettings b = brush(20);
    Selection sel;
    sel.selectRect(Rect{0, 0, 32, kH});  // left half only
    checkParity(
        "paint-selection",
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sl) {
            return paintStroke(d, l, s, Rgbaf{0.1f, 0.9f, 0.4f, 1.0f}, p, sl);
        },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sl) {
            return beginPaintStroke(d, l, s, Rgbaf{0.1f, 0.9f, 0.4f, 1.0f}, sl);
        },
        b, &sel, /*chunk=*/3);
}

PE_TEST(livestroke_cancel_restores) {
    LayerId l = kNoLayer;
    auto doc = gradientDoc(l);
    const std::vector<std::uint8_t> before = composite(*doc);
    auto stroke = beginEraseStroke(*doc, l, brush(18), nullptr);
    PE_CHECK(stroke != nullptr);
    std::vector<StrokePoint> acc;
    for (const StrokePoint& p : path()) {
        acc.push_back(p);
        (void)stroke->extend(acc);
    }
    PE_CHECK(composite(*doc) != before);  // the live preview changed the pixels
    stroke->cancel();
    PE_CHECK(composite(*doc) == before);  // cancel reverts to the pre-stroke state
}

PE_TEST(livestroke_no_spurious_tiles_on_absent_tile_erase) {
    // Regression guard for the flushTiles store-write gate: an erase dab whose footprint clips a
    // tile that is ABSENT at S0 and stays transparent must NOT materialize a tile in the live store
    // (the batched path leaves it absent), and undo must restore the exact pre-stroke tile set.
    // compositeImage parity is blind to this (a present all-transparent tile composites identically
    // to an absent one), so assert on tile occupancy directly.
    const BrushSettings b = brush(60);  // wide enough to spill across the tile boundary at x=256

    // A 400x64 doc spans tile columns 0 ([0,256)) and 1 ([256,400)). Content only in column 0.
    auto layout = [](Document& d, LayerId& l) {
        l = d.activeLayer();
        auto* pl = static_cast<PixelLayer*>(d.findLayer(l));
        pl->tiles().fillRect(Rect{0, 0, 200, 64}, Rgba8{200, 100, 50, 255});
    };
    // Stroke straddling the col0/col1 boundary; the part past x=200 (incl. all of column 1) erases
    // transparent pixels -> no change there, so column 1 must never be created.
    std::vector<StrokePoint> pts;
    for (int i = 0; i <= 20; ++i) {
        pts.push_back({{210.0f + static_cast<float>(i) * 4.5f, 32.0f}, 1.0f});
    }
    const TileCoord col1{1, 0};

    // Batched reference.
    auto da = Document::createBlank(Size{400, 64});
    LayerId la = kNoLayer;
    layout(*da, la);
    auto* pla = static_cast<PixelLayer*>(da->findLayer(la));
    const std::size_t preTiles = pla->tiles().tileCount();
    auto cmdA = eraseStroke(*da, la, b, pts, nullptr);
    if (cmdA != nullptr) cmdA->execute(*da);

    // Incremental.
    auto db = Document::createBlank(Size{400, 64});
    LayerId lb = kNoLayer;
    layout(*db, lb);
    auto* plb = static_cast<PixelLayer*>(db->findLayer(lb));
    auto stroke = beginEraseStroke(*db, lb, b, nullptr);
    PE_CHECK(stroke != nullptr);
    std::vector<StrokePoint> acc;
    for (const StrokePoint& p : pts) {
        acc.push_back(p);
        (void)stroke->extend(acc);
    }
    auto cmdB = stroke->finish();

    // Live store occupancy matches batched exactly — no spurious column-1 tile.
    PE_CHECK(plb->tiles().tileCount() == pla->tiles().tileCount());
    PE_CHECK(plb->tiles().hasTileAt(col1) == pla->tiles().hasTileAt(col1));
    PE_CHECK(!plb->tiles().hasTileAt(col1));

    // Undo restores the exact pre-stroke tile set (a spurious tile would survive otherwise).
    if (cmdB != nullptr) {
        db->history().push(std::move(cmdB));
        db->history().undo();
    }
    PE_CHECK(plb->tiles().tileCount() == preTiles);
    PE_CHECK(!plb->tiles().hasTileAt(col1));
}

PE_TEST(livestroke_clone_f32_matches_batched_native) {
    // Clone on an F32 layer with super-white content: the no-RGB-clamp source-over path must be
    // byte-identical between the live and batched strokes in the native store.
    const BrushSettings b = brush(16);
    const int offX = 12;
    const int offY = 8;
    checkDepthParity(
        hdrDocF32,
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) { return cloneStroke(d, l, s, p, offX, offY, sel); },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginCloneStroke(d, l, s, offX, offY, sel);
        },
        sampleF32, b, /*chunk=*/1);
}

PE_TEST(livestroke_dodge_f32_matches_batched_native) {
    // Dodge on F32: the max(0,1-dst) HDR clamp leaves super-white alone; live must match batched in
    // the native store (compositeImage would clamp and hide any divergence).
    const BrushSettings b = brush(16);
    checkDepthParity(
        hdrDocF32,
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) { return dodgeStroke(d, l, s, p, sel); },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginDodgeStroke(d, l, s, sel);
        },
        sampleF32, b, /*chunk=*/2);
}

PE_TEST(livestroke_paint_u16_matches_batched_native) {
    // Paint on a U16 layer: exercises the LiveStrokeImpl<Rgba16> instantiation and the U16 arm of
    // the beginLive() depth dispatch, byte-for-byte against the batched stroke in the native store.
    const BrushSettings b = brush(14);
    checkDepthParity(
        gradientDocU16,
        [](Document& d, LayerId l, const BrushSettings& s, std::span<const StrokePoint> p,
           const Selection* sel) {
            return paintStroke(d, l, s, Rgbaf{0.8f, 0.2f, 0.5f, 1.0f}, p, sel);
        },
        [](Document& d, LayerId l, const BrushSettings& s, const Selection* sel) {
            return beginPaintStroke(d, l, s, Rgbaf{0.8f, 0.2f, 0.5f, 1.0f}, sel);
        },
        sampleU16, b, /*chunk=*/3);
}
