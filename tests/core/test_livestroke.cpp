#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
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
