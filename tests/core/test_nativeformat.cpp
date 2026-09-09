#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/NativeFormat.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pe;

namespace {
PixelLayer* asPixel(Document& doc, std::size_t topIndex) {
    return dynamic_cast<PixelLayer*>(
        const_cast<Layer*>(doc.topLevelLayers()[topIndex].get()));  // NOLINT: test convenience
}

// --- helpers for the #173 solid-fill geometry cases ---------------------------------------

// A document whose only serializable content is one solid fill at `bounds`.
std::unique_ptr<Document> docWithSolid(Rect bounds, Rgba8 color, BitDepth depth = BitDepth::U8) {
    auto doc = Document::createBlank(Size{64, 48}, ColorMode::RGB, depth, 72);
    if (doc == nullptr) return nullptr;
    // Drop the seeded pixel layer so the solid record is the only layer in the file, which
    // keeps the byte offsets below unambiguous.
    std::vector<LayerId> seeded;
    for (const auto& l : doc->topLevelLayers()) seeded.push_back(l->id());
    for (LayerId id : seeded) (void)doc->cmdRemoveTopLevel(id);
    doc->cmdInsertTopLevel(0, std::make_unique<SolidColorLayer>(color, bounds, "Fill"));
    return doc;
}

const SolidColorLayer* firstSolid(const Document& doc) {
    for (const auto& l : doc.topLevelLayers()) {
        if (const auto* s = dynamic_cast<const SolidColorLayer*>(l.get())) return s;
    }
    return nullptr;
}

void putI32(std::vector<std::byte>& blob, std::size_t at, std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    blob[at] = static_cast<std::byte>(u & 0xFFu);
    blob[at + 1] = static_cast<std::byte>((u >> 8) & 0xFFu);
    blob[at + 2] = static_cast<std::byte>((u >> 16) & 0xFFu);
    blob[at + 3] = static_cast<std::byte>((u >> 24) & 0xFFu);
}

// Where `rect` was written, found by its own bytes rather than by a hand-derived offset:
// the layer record has grown twice already and a stale offset would silently patch the
// wrong field. Returns npos unless the pattern occurs exactly once.
std::size_t findRect(const std::vector<std::byte>& blob, Rect rect) {
    std::vector<std::byte> want(16);
    putI32(want, 0, rect.x);
    putI32(want, 4, rect.y);
    putI32(want, 8, rect.width);
    putI32(want, 12, rect.height);
    std::size_t found = static_cast<std::size_t>(-1);
    std::size_t hits = 0;
    if (blob.size() < want.size()) return static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i + want.size() <= blob.size(); ++i) {
        if (std::equal(want.begin(), want.end(), blob.begin() + static_cast<std::ptrdiff_t>(i))) {
            ++hits;
            found = i;
        }
    }
    return hits == 1 ? found : static_cast<std::size_t>(-1);
}

// Relabel a v6 blob as v7 (both the digit and the authoritative u32).
void declareV7(std::vector<std::byte>& blob) {
    blob[5] = std::byte{'7'};
    blob[6] = std::byte{7};
}
}  // namespace

PE_TEST(native_format_roundtrip_preserves_layers) {
    auto doc = Document::createBlank(Size{32, 24});
    auto* base = asPixel(*doc, 0);
    base->setName("Base");
    base->tiles().fillRect(Rect{0, 0, 32, 24}, Rgba8{20, 40, 60, 255});

    auto top = std::make_unique<PixelLayer>("Top");
    top->setOpacity(0.5f);
    top->setBlendMode(BlendMode::Multiply);
    top->setVisible(false);
    top->tiles().setPixel(10, 8, Rgba8{200, 100, 50, 128});
    const LayerId topId = top->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(top));
    doc->setActiveLayer(topId);

    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(!blob.empty());

    auto loaded = deserializeDocument(blob);
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->canvasSize().width, 32);
    PE_CHECK_EQ(loaded->canvasSize().height, 24);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));

    auto* lbase = asPixel(*loaded, 0);
    auto* ltop = asPixel(*loaded, 1);
    PE_CHECK(lbase != nullptr && ltop != nullptr);
    PE_CHECK_EQ(lbase->name(), std::string("Base"));
    PE_CHECK_EQ(lbase->tiles().pixel(0, 0), (Rgba8{20, 40, 60, 255}));

    PE_CHECK_EQ(ltop->name(), std::string("Top"));
    PE_CHECK_EQ(ltop->visible(), false);
    PE_CHECK_NEAR(ltop->opacity(), 0.5f);
    PE_CHECK(ltop->blendMode() == BlendMode::Multiply);
    PE_CHECK_EQ(ltop->tiles().pixel(10, 8), (Rgba8{200, 100, 50, 128}));
    PE_CHECK_EQ(ltop->tiles().pixel(0, 0), (Rgba8{0, 0, 0, 0}));  // empty elsewhere

    // The active layer is restored (by index, since ids are session-local).
    PE_CHECK_EQ(loaded->activeLayer(), ltop->id());
}

PE_TEST(native_format_rejects_garbage_and_truncation) {
    std::vector<std::byte> junk(40, std::byte{0x33});
    PE_CHECK(deserializeDocument(junk) == nullptr);                          // bad magic
    PE_CHECK(deserializeDocument(std::span<const std::byte>{}) == nullptr);  // empty

    // A valid blob truncated at every length must never crash and must reject.
    auto doc = Document::createBlank(Size{8, 8});
    asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{1, 2, 3, 255});
    std::vector<std::byte> blob = serializeDocument(*doc);
    for (std::size_t n = 0; n < blob.size(); ++n) {
        auto truncated = deserializeDocument(std::span<const std::byte>(blob.data(), n));
        PE_CHECK(truncated == nullptr);  // every short read is rejected, not crashed
    }
    PE_CHECK(deserializeDocument(blob) != nullptr);  // the full blob still loads
}

PE_TEST(native_format_empty_layer_roundtrip) {
    auto doc = Document::createBlank(Size{16, 16});  // single empty layer
    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(asPixel(*loaded, 0)->tiles().pixel(0, 0), (Rgba8{0, 0, 0, 0}));
}

PE_TEST(native_format_roundtrip_adjustment_layer) {
    // An adjustment layer (non-destructive) must survive save/load with its parameters — the
    // data-loss gap this fixes. Use Levels (non-default params via the new getters).
    auto doc = Document::createBlank(Size{16, 16});
    auto lv = std::make_unique<Levels>();
    lv->setInputBlack(0.2f);
    lv->setGamma(2.5f);
    lv->setOutputWhite(0.8f);
    auto adj = std::make_unique<AdjustmentLayer>(std::move(lv), "My Levels");
    adj->setOpacity(0.75f);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));
    const Layer* l = loaded->topLevelLayers()[1].get();
    PE_CHECK(l != nullptr && l->isAdjustment());
    PE_CHECK_EQ(l->name(), std::string("My Levels"));
    PE_CHECK_NEAR(l->opacity(), 0.75f);
    const auto& a = static_cast<const AdjustmentLayer*>(l)->adjustment();
    PE_CHECK(a.kind() == AdjustmentKind::Levels);
    const auto& lvl = static_cast<const Levels&>(a);
    PE_CHECK_NEAR(lvl.inputBlack(), 0.2f);
    PE_CHECK_NEAR(lvl.gamma(), 2.5f);
    PE_CHECK_NEAR(lvl.outputWhite(), 0.8f);
}

PE_TEST(native_format_roundtrip_solid_color_layer) {
    auto doc = Document::createBlank(Size{32, 32});
    doc->cmdInsertTopLevel(
        doc->topLevelCount(),
        std::make_unique<SolidColorLayer>(Rgba8{10, 20, 30, 200}, Rect{4, 5, 16, 12}, "Fill"));

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));
    const auto* s = dynamic_cast<const SolidColorLayer*>(loaded->topLevelLayers()[1].get());
    PE_REQUIRE(s != nullptr);
    PE_CHECK_EQ(s->color(), (Rgba8{10, 20, 30, 200}));
    PE_CHECK_EQ(s->bounds(), (Rect{4, 5, 16, 12}));
}

PE_TEST(native_format_roundtrip_all_adjustment_kinds) {
    // Every serialized AdjustmentKind survives the round-trip with the right kind tag.
    auto doc = Document::createBlank(Size{8, 8});
    std::vector<std::unique_ptr<Adjustment>> kinds;
    kinds.push_back(std::make_unique<BrightnessContrast>(0.3f, -0.2f));
    kinds.push_back(std::make_unique<Curves>());
    kinds.push_back(std::make_unique<Invert>());
    kinds.push_back(std::make_unique<Exposure>(1.0f, 0.1f, 1.2f));
    kinds.push_back(std::make_unique<HueSaturation>());
    kinds.push_back(std::make_unique<ChannelMixer>());
    kinds.push_back(std::make_unique<GradientMap>());
    kinds.push_back(std::make_unique<Vibrance>(0.4f, -0.3f));
    kinds.push_back(std::make_unique<ColorBalance>());
    kinds.push_back(std::make_unique<BlackAndWhite>());
    kinds.push_back(std::make_unique<PhotoFilter>());
    kinds.push_back(std::make_unique<Posterize>(6));
    kinds.push_back(std::make_unique<Threshold>(0.4f));
    kinds.push_back(std::make_unique<SelectiveColor>());
    std::vector<AdjustmentKind> expected;
    for (auto& k : kinds) {
        expected.push_back(k->kind());
        doc->cmdInsertTopLevel(doc->topLevelCount(),
                               std::make_unique<AdjustmentLayer>(std::move(k), "adj"));
    }

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), expected.size() + 1);  // +1 base pixel layer
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const Layer* l = loaded->topLevelLayers()[i + 1].get();
        PE_CHECK(l->isAdjustment());
        PE_CHECK(static_cast<const AdjustmentLayer*>(l)->adjustment().kind() == expected[i]);
    }
}

PE_TEST(native_format_accepts_legacy_v4) {
    // A v4 file (no adjustment/solid records) must still load: patch the version digit + field
    // of a pixel-only v5 blob down to 4 and confirm the reader accepts it.
    auto doc = Document::createBlank(Size{16, 16});
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(blob.size() > 10);
    blob[5] = std::byte{'4'};  // magic digit "PEDOC4"
    blob[6] = std::byte{4};    // version u32 little-endian = 4
    blob[7] = std::byte{0};
    blob[8] = std::byte{0};
    blob[9] = std::byte{0};
    auto loaded = deserializeDocument(blob);
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(1));
    // And an unsupported future version is rejected.
    blob[6] = std::byte{99};
    PE_CHECK(deserializeDocument(blob) == nullptr);
}

PE_TEST(native_format_rejects_bad_adjustment_kind) {
    // An Invert adjustment record ends with its AdjustmentKind byte (Invert writes no params), so
    // it's the blob's last byte. Corrupting it to an out-of-range kind must be rejected (no UB).
    auto doc = Document::createBlank(Size{8, 8});
    doc->cmdInsertTopLevel(doc->topLevelCount(),
                           std::make_unique<AdjustmentLayer>(std::make_unique<Invert>(), "inv"));
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(deserializeDocument(blob) != nullptr);  // valid as-is
    PE_CHECK_EQ(static_cast<int>(blob.back()), 3);   // Invert kind tag
    blob.back() = std::byte{0xFF};                   // invalid AdjustmentKind
    PE_CHECK(deserializeDocument(blob) == nullptr);  // rejected, no UB
}

PE_TEST(native_format_rejects_off_canvas_content_rect) {
    // A hostile file could set an extreme content-rect origin; deserialize must reject
    // it (not overflow the int loop bounds / over-allocate tiles). Build a deterministic
    // blob (empty layer name) and patch the content-rect Y to a far off-canvas origin.
    auto doc = Document::createBlank(Size{4, 4});
    auto* base = asPixel(*doc, 0);
    base->setName("");  // nameLen 0 -> known field offsets
    base->tiles().setPixel(0, 0, Rgba8{9, 9, 9, 255});
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(deserializeDocument(blob) != nullptr);  // sanity: valid as-is

    // v4 layout: header 28 + kind(1)+visible(1)+opacity(4)+blend(1)+active(1)+nameLen(4)=12
    // + hasMask(1) -> cx@41, cy@45.
    PE_CHECK(blob.size() > 49);
    blob[45] = std::byte{0xF0};
    blob[46] = std::byte{0xFF};
    blob[47] = std::byte{0xFF};
    blob[48] = std::byte{0x7F};                      // cy ~= 2.1e9, far outside the 4x4 canvas
    PE_CHECK(deserializeDocument(blob) == nullptr);  // rejected, no UB / over-allocation
}

PE_TEST(native_format_roundtrips_nested_groups) {
    auto doc = Document::createBlank(Size{16, 16});
    asPixel(*doc, 0)->setName("Bg");

    auto group = std::make_unique<GroupLayer>("Grp");
    group->setOpacity(0.7f);
    group->setIsolated(false);
    auto child = std::make_unique<PixelLayer>("Inner");
    child->setBlendMode(BlendMode::Screen);
    child->tiles().setPixel(3, 3, Rgba8{77, 88, 99, 255});
    const LayerId childId = child->id();
    group->addChild(std::move(child));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
    doc->setActiveLayer(childId);  // active layer is nested inside the group

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));

    const auto& tops = loaded->topLevelLayers();
    auto* lgrp = dynamic_cast<GroupLayer*>(const_cast<Layer*>(tops[1].get()));
    PE_REQUIRE(lgrp != nullptr);
    PE_CHECK_EQ(lgrp->name(), std::string("Grp"));
    PE_CHECK_NEAR(lgrp->opacity(), 0.7f);
    PE_CHECK_EQ(lgrp->isolated(), false);
    PE_CHECK_EQ(lgrp->childCount(), static_cast<std::size_t>(1));

    auto* linner = dynamic_cast<PixelLayer*>(const_cast<Layer*>(lgrp->children()[0].get()));
    PE_REQUIRE(linner != nullptr);
    PE_CHECK_EQ(linner->name(), std::string("Inner"));
    PE_CHECK(linner->blendMode() == BlendMode::Screen);
    PE_CHECK_EQ(linner->tiles().pixel(3, 3), (Rgba8{77, 88, 99, 255}));
    // The nested active layer is restored.
    PE_CHECK_EQ(loaded->activeLayer(), linner->id());
}

PE_TEST(native_format_rejects_excessive_group_nesting) {
    // A maliciously deep group chain must be rejected by the recursion cap rather than
    // overflowing the stack during deserialize.
    auto doc = Document::createBlank(Size{8, 8});
    std::unique_ptr<Layer> chain = std::make_unique<GroupLayer>("g");
    for (int i = 0; i < 300; ++i) {  // 300 > kMaxGroupDepth (256)
        auto outer = std::make_unique<GroupLayer>("g");
        outer->addChild(std::move(chain));
        chain = std::move(outer);
    }
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(chain));
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(deserializeDocument(blob) == nullptr);  // depth cap -> reject, no overflow
}

PE_TEST(native_format_pixel_compression_roundtrip) {
    // A large, highly-compressible solid layer round-trips exactly and (with zlib)
    // serializes far smaller than its raw RGBA payload.
    auto doc = Document::createBlank(Size{128, 128});
    auto* base = asPixel(*doc, 0);
    base->tiles().fillRect(Rect{0, 0, 128, 128}, Rgba8{50, 100, 150, 255});

    std::vector<std::byte> blob = serializeDocument(*doc);
    auto loaded = deserializeDocument(blob);
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(asPixel(*loaded, 0)->tiles().pixel(64, 64), (Rgba8{50, 100, 150, 255}));
    PE_CHECK_EQ(asPixel(*loaded, 0)->tiles().pixel(0, 0), (Rgba8{50, 100, 150, 255}));

#ifdef PHOTOEDIT_HAVE_ZLIB
    // 128*128*4 = 65536 raw pixel bytes; a solid fill must deflate dramatically.
    PE_CHECK(blob.size() < static_cast<std::size_t>(8192));
#endif
}

PE_TEST(native_format_roundtrips_layer_mask) {
    auto doc = Document::createBlank(Size{32, 32});
    auto* base = asPixel(*doc, 0);
    base->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{200, 50, 50, 255});

    auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
    mask->setEnabled(true);
    mask->setDensity(0.6f);
    mask->setInverted(true);
    mask->buffer().fillRect(Rect{4, 4, 8, 8}, MaskBuffer::kClear);  // hide a square
    mask->buffer().setValue(20, 20, 128);                           // a partial-coverage pixel
    base->setMask(std::move(mask));

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    auto* lbase = asPixel(*loaded, 0);
    const Mask* lm = lbase->mask();
    PE_REQUIRE(lm != nullptr);
    PE_CHECK(lm->kind() == Mask::Kind::Layer);
    PE_CHECK_EQ(lm->enabled(), true);
    PE_CHECK_NEAR(lm->density(), 0.6f);
    PE_CHECK_EQ(lm->inverted(), true);
    PE_CHECK_EQ(lm->buffer().value(6, 6), static_cast<uint8_t>(MaskBuffer::kClear));
    PE_CHECK_EQ(lm->buffer().value(20, 20), static_cast<uint8_t>(128));
    PE_CHECK_EQ(lm->buffer().value(0, 0),
                static_cast<uint8_t>(MaskBuffer::kOpaque));  // absent -> reveals
}

PE_TEST(native_format_no_mask_when_absent) {
    auto doc = Document::createBlank(Size{8, 8});  // layer has no mask
    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK(asPixel(*loaded, 0)->mask() == nullptr);
}

PE_TEST(native_format_roundtrip_16bit) {
    // A 16-bit document must round-trip its 16-bit store exactly. Before the bit-depth
    // fix the writer only serialized the (empty) 8-bit store, so a 16-bit doc loaded back
    // fully transparent — silent data loss.
    auto doc = Document::createBlank(Size{16, 16}, ColorMode::RGB, BitDepth::U16);
    PE_CHECK(doc->bitDepth() == BitDepth::U16);
    auto* base = asPixel(*doc, 0);
    PE_CHECK(base->depth() == BitDepth::U16);
    base->tiles16().setPixel(4, 5, Rgba16{1000, 40000, 65535, 65535});

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK(loaded->bitDepth() == BitDepth::U16);
    auto* lbase = asPixel(*loaded, 0);
    PE_CHECK(lbase != nullptr && lbase->depth() == BitDepth::U16);
    PE_CHECK_EQ(lbase->tiles16().pixel(4, 5), (Rgba16{1000, 40000, 65535, 65535}));
    PE_CHECK_EQ(lbase->tiles16().pixel(0, 0), (Rgba16{0, 0, 0, 0}));  // empty elsewhere
    // The 8-bit store stays empty for a 16-bit layer.
    PE_CHECK_EQ(lbase->tiles().pixel(4, 5), (Rgba8{0, 0, 0, 0}));
}

PE_TEST(native_format_roundtrip_32bit) {
    // A 32-bit-float document round-trips its float store bit-exactly (values written and
    // read via raw bytes). Exactly-representable values let us compare without tolerance.
    auto doc = Document::createBlank(Size{16, 16}, ColorMode::RGB, BitDepth::F32);
    PE_CHECK(doc->bitDepth() == BitDepth::F32);
    auto* base = asPixel(*doc, 0);
    PE_CHECK(base->depth() == BitDepth::F32);
    base->tilesF().setPixel(7, 2, Rgbaf{0.25f, 0.5f, 0.75f, 1.0f});

    auto loaded = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK(loaded->bitDepth() == BitDepth::F32);
    auto* lbase = asPixel(*loaded, 0);
    PE_CHECK(lbase != nullptr && lbase->depth() == BitDepth::F32);
    const Rgbaf px = lbase->tilesF().pixel(7, 2);
    PE_CHECK_NEAR(px.r, 0.25f);
    PE_CHECK_NEAR(px.g, 0.5f);
    PE_CHECK_NEAR(px.b, 0.75f);
    PE_CHECK_NEAR(px.a, 1.0f);
    const Rgbaf empty = lbase->tilesF().pixel(0, 0);
    PE_CHECK_NEAR(empty.a, 0.0f);  // transparent elsewhere
}

PE_TEST(native_format_budget_rejects_excess_allocation) {
    // A crafted/large file must not be able to drive unbounded allocation. The aggregate
    // memory budget is checked before each block is allocated: a tight budget rejects the
    // file (nullptr) instead of materializing the pixels; the default budget accepts it.
    // Each 8x8 layer touches one 256x256 tile => 65536*4 = 262144 B resident footprint.
    auto doc = Document::createBlank(Size{8, 8});
    asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{10, 20, 30, 255});
    auto second = std::make_unique<PixelLayer>("Two");
    second->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{40, 50, 60, 255});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(second));

    const std::vector<std::byte> blob = serializeDocument(*doc);

    // Fits one layer's 262144 B footprint but not both (aggregate across siblings): reject.
    PE_CHECK(deserializeDocument(blob, 400'000) == nullptr);
    // Zero budget rejects even the first non-empty block.
    PE_CHECK(deserializeDocument(blob, 0) == nullptr);
    // The default (generous) budget loads it fine — the cap only bites pathological input.
    auto loaded = deserializeDocument(blob);
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));
    PE_CHECK_EQ(asPixel(*loaded, 1)->tiles().pixel(0, 0), (Rgba8{40, 50, 60, 255}));
}

PE_TEST(native_format_budget_charges_tiled_footprint_not_dense) {
    // Regression: the budget must charge the RESIDENT tiled footprint, not the dense rect
    // byte count. Pixels scatter into a sparse 256x256-tiled store, so a thin/wide rect
    // materializes whole tiles far exceeding its packed size — charging dense bytes would
    // let such a strip bypass the cap ~256x. A 2560x2 fill packs to ~20 KB but spans 10
    // tiles => 10*65536*4 = ~2.62 MB resident.
    auto doc = Document::createBlank(Size{2560, 2});
    asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 2560, 2}, Rgba8{1, 2, 3, 255});
    const std::vector<std::byte> blob = serializeDocument(*doc);

    // 1 MB sits ABOVE the dense bytes (~20 KB) but BELOW the tiled footprint (~2.62 MB):
    // it must reject. (With the old dense accounting this strip would have loaded.)
    PE_CHECK(deserializeDocument(blob, 1'000'000) == nullptr);
    // The default budget comfortably covers ~2.62 MB, so the file still round-trips.
    auto loaded = deserializeDocument(blob);
    PE_REQUIRE(loaded != nullptr);
    PE_CHECK_EQ(asPixel(*loaded, 0)->tiles().pixel(1000, 0), (Rgba8{1, 2, 3, 255}));
}

PE_TEST(native_format_budget_charged_for_mask_block) {
    // The mask block flows through the same budgeted chokepoint. An 8x8 mask spans one
    // 1-byte tile => 65536 B footprint; a budget below that must reject even though the
    // (empty) pixel layer itself costs nothing.
    auto doc = Document::createBlank(Size{8, 8});  // base layer left empty (0 pixel bytes)
    auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
    mask->buffer().fillRect(Rect{0, 0, 8, 8}, MaskBuffer::kClear);  // materialize a mask tile
    asPixel(*doc, 0)->setMask(std::move(mask));
    const std::vector<std::byte> blob = serializeDocument(*doc);

    PE_CHECK(deserializeDocument(blob, 1'000) == nullptr);  // 65536 B mask > 1000 -> reject
    PE_CHECK(deserializeDocument(blob) != nullptr);         // default budget loads it
}

PE_TEST(native_format_budget_shared_across_group_children) {
    // The budget is shared by reference through group recursion, so a group whose children
    // collectively exceed it is rejected even if each child fits alone. Two filled 8x8
    // children => 262144 B footprint each; a budget between one and two must reject.
    auto doc = Document::createBlank(Size{8, 8});  // base layer empty
    auto group = std::make_unique<GroupLayer>("Grp");
    for (int i = 0; i < 2; ++i) {
        auto child = std::make_unique<PixelLayer>("Child");
        child->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{static_cast<std::uint8_t>(i), 0, 0, 255});
        group->addChild(std::move(child));
    }
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
    const std::vector<std::byte> blob = serializeDocument(*doc);

    // Fits one child (262144) but not both: reject — proving the budget is not reset per child.
    PE_CHECK(deserializeDocument(blob, 400'000) == nullptr);
    PE_CHECK(deserializeDocument(blob) != nullptr);  // default budget loads it
}

PE_TEST(native_format_roundtrips_content_outside_the_canvas) {
    // The writer used to clamp every content rect to the canvas, so pixels a user had
    // moved past the edge were dropped at save time and could not be recovered by undo.
    // The engine supports off-canvas content deliberately elsewhere: floorDiv and
    // tileLocalOffset are correct for negative coordinates and tested, and Selection goes
    // out of its way to preserve off-canvas coverage.
    auto doc = Document::createBlank(Size{64, 64});
    auto* base = asPixel(*doc, 0);
    base->setName("Moved");
    base->tiles().fillRect(Rect{-40, -40, 60, 60}, Rgba8{7, 8, 9, 255});  // straddles the origin
    base->tiles().setPixel(-30, -30, Rgba8{200, 100, 50, 255});

    const std::vector<std::byte> blob = serializeDocument(*doc);
    auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    auto* rl = asPixel(*back, 0);
    PE_REQUIRE(rl != nullptr);

    PE_CHECK_EQ(rl->tiles().pixel(-30, -30), (Rgba8{200, 100, 50, 255}));  // off-canvas survives
    PE_CHECK_EQ(rl->tiles().pixel(-5, -5), (Rgba8{7, 8, 9, 255}));
    PE_CHECK_EQ(rl->tiles().pixel(10, 10), (Rgba8{7, 8, 9, 255}));  // on-canvas unchanged
    PE_CHECK_EQ(rl->tiles().pixel(30, 30), (Rgba8{}));              // never painted
}

PE_TEST(native_format_roundtrips_a_mask_outside_the_canvas) {
    auto doc = Document::createBlank(Size{64, 64});
    auto* base = asPixel(*doc, 0);
    base->setName("");
    base->tiles().fillRect(Rect{-40, -40, 120, 120}, Rgba8{7, 8, 9, 255});
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{-30, -30, 40, 40}, MaskBuffer::kClear);
    base->setMask(std::move(mask));

    auto back = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(back != nullptr);
    const Layer* rl = back->topLevelLayers()[0].get();
    PE_CHECK(rl->mask() != nullptr);
    PE_CHECK_EQ(rl->mask()->buffer().value(-20, -20), MaskBuffer::kClear);
    PE_CHECK_EQ(rl->mask()->buffer().value(40, 40), MaskBuffer::kOpaque);
}

PE_TEST(native_format_stays_on_the_old_version_when_nothing_is_off_canvas) {
    // Bumping the version on every save would stop older builds reading ordinary files.
    // The newer version is only written when a document actually needs it.
    auto doc = Document::createBlank(Size{32, 32});
    asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{1, 2, 3, 255});
    const std::vector<std::byte> ordinary = serializeDocument(*doc);
    PE_CHECK(ordinary.size() > 6);
    PE_CHECK_EQ(static_cast<int>(ordinary[5]), static_cast<int>(std::byte{'6'}));

    asPixel(*doc, 0)->tiles().setPixel(-1, -1, Rgba8{4, 5, 6, 255});
    const std::vector<std::byte> offCanvas = serializeDocument(*doc);
    PE_CHECK_EQ(static_cast<int>(offCanvas[5]), static_cast<int>(std::byte{'7'}));
    PE_CHECK(deserializeDocument(offCanvas) != nullptr);
}

PE_TEST(native_format_stays_on_the_old_version_with_an_ordinary_mask) {
    // MaskBuffer::contentBounds() is tile-granular like TileStoreT's, so a mask painted
    // anywhere inside a small canvas reports bounds that overhang it. Writing that
    // verbatim would bloat every masked file to whole tiles and, worse, make every
    // masked document claim the newer version and stop older builds reading it.
    auto doc = Document::createBlank(Size{32, 32});
    auto* base = asPixel(*doc, 0);
    base->setName("");
    base->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{1, 2, 3, 255});
    auto mask = std::make_unique<Mask>();
    mask->buffer().fillRect(Rect{4, 4, 8, 8}, MaskBuffer::kClear);  // well inside the canvas
    base->setMask(std::move(mask));

    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(blob.size() > 6);
    PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'6'}));

    auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    const Layer* rl = back->topLevelLayers()[0].get();
    PE_CHECK(rl->mask() != nullptr);
    PE_CHECK_EQ(rl->mask()->buffer().value(6, 6), MaskBuffer::kClear);
    PE_CHECK_EQ(rl->mask()->buffer().value(20, 20), MaskBuffer::kOpaque);
}

PE_TEST(native_format_still_rejects_off_canvas_in_a_pre_v7_file) {
    // Off-canvas content rects are legal only from v7. A v6 file could never contain one,
    // so a v6 record claiming a negative origin is malformed and must still be refused:
    // relaxing the check for every version would widen what a legacy file may claim.
    // The existing extreme-origin test does not cover this, because 2.1e9 is out of range
    // under the v7 rules too and so is rejected either way.
    auto doc = Document::createBlank(Size{16, 16});
    auto* base = asPixel(*doc, 0);
    base->setName("");  // nameLen 0 -> known field offsets
    base->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{9, 9, 9, 255});
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'6'}));  // nothing off-canvas
    PE_CHECK(deserializeDocument(blob) != nullptr);

    // cy lives at byte 45; make it -8, which is a plausible v7 value and illegal in v6.
    PE_CHECK(blob.size() > 49);
    blob[45] = std::byte{0xF8};
    blob[46] = std::byte{0xFF};
    blob[47] = std::byte{0xFF};
    blob[48] = std::byte{0xFF};
    PE_CHECK(deserializeDocument(blob) == nullptr);

    // The same record is accepted once the file declares v7.
    blob[5] = std::byte{'7'};
    blob[6] = std::byte{7};  // the u32 version field is authoritative
    PE_CHECK(deserializeDocument(blob) != nullptr);
}

PE_TEST(native_format_rejects_an_off_canvas_rect_beyond_the_coordinate_range) {
    // v7 permits a negative content origin, but only within the engine's representable
    // canvas range, so the int loop bounds and the tile math cannot overflow. The origin
    // is moved without changing cw or ch, so the pixel block that follows still has the
    // length the reader expects and the content rect is the only thing under test.
    //
    // Allocation itself is bounded elsewhere and always was: readBlock charges the
    // aggregate budget the resident tiled footprint rather than the packed rect size, so
    // a thin strip cannot commit more memory than its pixel count suggests.
    auto doc = Document::createBlank(Size{16, 16});
    auto* base = asPixel(*doc, 0);
    base->setName("");
    base->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{9, 9, 9, 255});
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(blob.size() > 57);
    blob[5] = std::byte{'7'};  // declare the version that permits off-canvas origins
    blob[6] = std::byte{7};
    PE_CHECK(deserializeDocument(blob) != nullptr);  // the relabel alone changes nothing

    const auto put = [&blob](std::size_t at, std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int i = 0; i < 4; ++i) {
            blob[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((u >> (8 * i)) & 0xFF);
        }
    };
    put(41, -1000);  // comfortably off-canvas, well inside the range: accepted
    PE_CHECK(deserializeDocument(blob) != nullptr);

    put(41, -300001);  // one past kMaxCanvasDimension: refused
    PE_CHECK(deserializeDocument(blob) == nullptr);
}

PE_TEST(native_format_round_trips_a_solid_fill_that_extends_past_every_edge) {
    // #173. The writer emits a solid layer's rect unclamped, and hasOffCanvasContent bumps
    // the file to v7 for it, but the reader validated the rect against the canvas anyway.
    // A document with an off-canvas fill therefore SAVED successfully and could not be
    // opened, losing everything in it, not just that layer.
    //
    // Every edge, and a corner, because the old check rejected on four separate conditions
    // and fixing one of them would leave the rest.
    const Rect cases[] = {
        {-20, 10, 30, 20},     // past the left edge
        {10, -15, 20, 30},     // past the top
        {50, 10, 40, 20},      // past the right (canvas is 64 wide)
        {10, 40, 20, 30},      // past the bottom (canvas is 48 tall)
        {-8, -8, 16, 16},      // the top-left corner, two edges at once
        {-99, -99, 400, 400},  // strictly larger than the canvas in every direction
    };
    for (const Rect bounds : cases) {
        for (const BitDepth depth : {BitDepth::U8, BitDepth::U16, BitDepth::F32}) {
            const Rgba8 color{200, 30, 40, 128};
            auto doc = docWithSolid(bounds, color, depth);
            PE_CHECK(doc != nullptr);
            if (doc == nullptr) continue;

            const std::vector<std::byte> blob = serializeDocument(*doc);
            PE_CHECK(!blob.empty());
            // It really is the off-canvas format: the version is the evidence that the
            // writer considered this rect off-canvas rather than quietly clamping it.
            PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'7'}));

            const auto back = deserializeDocument(blob);
            PE_CHECK(back != nullptr);
            if (back == nullptr) continue;
            const SolidColorLayer* solid = firstSolid(*back);
            PE_CHECK(solid != nullptr);
            if (solid == nullptr) continue;
            PE_CHECK(solid->bounds() == bounds);  // unclamped, exactly as written
            PE_CHECK(solid->color() == color);
            PE_CHECK(solid->name() == "Fill");
            PE_CHECK_EQ(back->topLevelCount(), static_cast<std::size_t>(1));

            // And it is stable: a second round trip produces the same bytes, so the reader
            // did not merely accept the rect but store something else.
            PE_CHECK(serializeDocument(*back) == blob);
        }
    }
}

PE_TEST(native_format_keeps_an_on_canvas_solid_fill_on_the_older_version) {
    // The other half of #142's bargain, which must not regress: a document with nothing
    // off-canvas still writes v6, so builds that predate v7 keep reading ordinary files.
    auto doc = docWithSolid(Rect{10, 10, 20, 20}, Rgba8{1, 2, 3, 255});
    PE_CHECK(doc != nullptr);
    if (doc == nullptr) return;
    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'6'}));
    const auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    if (back != nullptr) {
        const SolidColorLayer* solid = firstSolid(*back);
        PE_CHECK(solid != nullptr);
        if (solid != nullptr) PE_CHECK(solid->bounds() == Rect{10, 10, 20, 20});
    }
}

PE_TEST(native_format_still_rejects_an_off_canvas_solid_fill_in_a_pre_v7_file) {
    // Outside the canvas is legal from v7 only. A v6 file could never contain such a rect,
    // so widening the check for every version would widen what a legacy file may claim.
    auto doc = docWithSolid(Rect{10, 10, 20, 20}, Rgba8{1, 2, 3, 255});
    PE_CHECK(doc != nullptr);
    if (doc == nullptr) return;
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'6'}));
    const std::size_t at = findRect(blob, Rect{10, 10, 20, 20});
    PE_CHECK(at != static_cast<std::size_t>(-1));
    if (at == static_cast<std::size_t>(-1)) return;

    putI32(blob, at, -4);  // a plausible v7 origin, illegal in v6
    PE_CHECK(deserializeDocument(blob) == nullptr);

    // The same record is accepted once the file declares the version that permits it.
    declareV7(blob);
    const auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    if (back != nullptr) {
        const SolidColorLayer* solid = firstSolid(*back);
        PE_CHECK(solid != nullptr);
        if (solid != nullptr) PE_CHECK(solid->bounds() == Rect{-4, 10, 20, 20});
    }
}

PE_TEST(native_format_rejects_a_solid_fill_rect_that_is_not_representable) {
    // Widening the reader to accept off-canvas geometry must not widen it to accept
    // geometry the engine cannot hold. Rect::right() is x + width in int, so a rect whose
    // far edge leaves the representable range would overflow on the first use.
    //
    // Each case is patched into an otherwise valid v7 file, so the rect is the only thing
    // under test and the record that follows still has the length the reader expects.
    const Rect start{10, 10, 20, 20};
    const Rect bad[] = {
        {2'000'000'000, 10, 20, 20},   // origin past the coordinate range
        {10, 2'000'000'000, 20, 20},   // the same on y
        {-2'000'000'000, 10, 20, 20},  // and negatively
        {10, 10, 2'000'000'000, 20},   // far edge past the range: x + width overflows
        {10, 10, 20, 2'000'000'000},   // the same on the other axis
        {10, 10, -1, 20},              // negative extent
        {10, 10, 20, -1},
    };
    for (const Rect r : bad) {
        auto doc = docWithSolid(start, Rgba8{1, 2, 3, 255});
        PE_CHECK(doc != nullptr);
        if (doc == nullptr) continue;
        std::vector<std::byte> blob = serializeDocument(*doc);
        declareV7(blob);  // the version that permits off-canvas, so only the rect is on trial
        const std::size_t at = findRect(blob, start);
        PE_CHECK(at != static_cast<std::size_t>(-1));
        if (at == static_cast<std::size_t>(-1)) continue;
        PE_CHECK(deserializeDocument(blob) != nullptr);  // the relabel alone changes nothing

        putI32(blob, at, r.x);
        putI32(blob, at + 4, r.y);
        putI32(blob, at + 8, r.width);
        putI32(blob, at + 12, r.height);
        // Cleanly: a null return through the normal API, not a crash or a hang.
        PE_CHECK(deserializeDocument(blob) == nullptr);
    }
}

PE_TEST(native_format_accepts_a_solid_fill_of_any_area) {
    // A solid fill is procedural: it stores four numbers and renders by intersecting each
    // tile, so it allocates nothing proportional to its area. It was the first record kind
    // to prove the reader's per-layer AREA cap was the wrong tool; that cap is gone now for
    // every record kind, and memory is bounded by the aggregate budget instead. This still
    // pins the cheap-whatever-the-area property, which is a fact about the record.
    auto doc = Document::createBlank(Size{20'000, 20'000});  // 400 MP
    PE_CHECK(doc != nullptr);
    if (doc == nullptr) return;
    std::vector<LayerId> seeded;
    for (const auto& l : doc->topLevelLayers()) seeded.push_back(l->id());
    for (LayerId id : seeded) (void)doc->cmdRemoveTopLevel(id);
    doc->cmdInsertTopLevel(0, std::make_unique<SolidColorLayer>(Rgba8{9, 8, 7, 255},
                                                                Rect{0, 0, 20'000, 20'000}, "Big"));
    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(blob.size() < 1000);  // procedural: the file is tiny whatever the area
    const auto back = deserializeDocument(blob);
    PE_CHECK(back != nullptr);
    if (back == nullptr) return;
    const SolidColorLayer* solid = firstSolid(*back);
    PE_REQUIRE(solid != nullptr);
    if (solid != nullptr) PE_CHECK(solid->bounds() == Rect{0, 0, 20'000, 20'000});
}

PE_TEST(native_format_scans_each_persisted_store_exactly_once_per_save) {
    // #174. Content bounds are O(pixels in the layer) and two consumers want the same
    // answer: the version decision asks whether anything is off-canvas, and the record
    // emission writes the rect it found. They used to compute it separately, so an
    // ordinary save scanned every pixel of every layer twice.
    //
    // Asserted as a count rather than as wall time. The duplicate is worth about 3.5% of a
    // 24 MP save, which a benchmark on a fast machine would not reliably notice, so a
    // regression would sail through CI unremarked.
    const auto scansFor = [](const Document& doc) {
        const std::uint64_t before = contentBoundsScanCount();
        (void)serializeDocument(doc);
        return contentBoundsScanCount() - before;
    };

    {  // Three pixel layers, no masks: three dense stores, three scans.
        auto doc = Document::createBlank(Size{200, 150});
        asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 200, 150}, Rgba8{1, 2, 3, 255});
        for (int i = 0; i < 2; ++i) {
            auto extra = std::make_unique<PixelLayer>("L", BitDepth::U8);
            extra->tiles().fillRect(Rect{10, 10, 100, 100}, Rgba8{4, 5, 6, 255});
            doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(extra));
        }
        PE_CHECK_EQ(scansFor(*doc), static_cast<std::uint64_t>(3));
    }

    {  // Add a mask to each: a mask buffer is a second persisted store, scanned once too.
        auto doc = Document::createBlank(Size{200, 150});
        for (int i = 0; i < 3; ++i) {
            PixelLayer* pl = nullptr;
            std::unique_ptr<PixelLayer> made;
            if (i == 0) {
                pl = asPixel(*doc, 0);
            } else {
                made = std::make_unique<PixelLayer>("L", BitDepth::U8);
                pl = made.get();
            }
            pl->tiles().fillRect(Rect{0, 0, 200, 150}, Rgba8{1, 2, 3, 255});
            auto m = std::make_unique<Mask>();
            m->buffer().fillRect(Rect{5, 5, 50, 50}, MaskBuffer::kClear);
            pl->setMask(std::move(m));
            if (made != nullptr) doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(made));
        }
        PE_CHECK_EQ(scansFor(*doc), static_cast<std::uint64_t>(6));
    }

    {  // Nesting does not multiply the count: one scan per store, wherever it sits.
        auto doc = Document::createBlank(Size{200, 150});
        asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 200, 150}, Rgba8{1, 2, 3, 255});
        auto group = std::make_unique<GroupLayer>("Set");
        auto inner = std::make_unique<PixelLayer>("Inner", BitDepth::U8);
        inner->tiles().fillRect(Rect{0, 0, 100, 100}, Rgba8{7, 7, 7, 255});
        auto nested = std::make_unique<GroupLayer>("Nested");
        auto deep = std::make_unique<PixelLayer>("Deep", BitDepth::U8);
        deep->tiles().fillRect(Rect{20, 20, 40, 40}, Rgba8{8, 8, 8, 255});
        nested->addChild(std::move(deep));
        group->addChild(std::move(inner));
        group->addChild(std::move(nested));
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
        PE_CHECK_EQ(scansFor(*doc), static_cast<std::uint64_t>(3));
    }

    {  // A solid fill is procedural: no dense store, so nothing to scan at all.
        auto doc = Document::createBlank(Size{200, 150});
        std::vector<LayerId> seeded;
        for (const auto& l : doc->topLevelLayers()) seeded.push_back(l->id());
        for (LayerId id : seeded) (void)doc->cmdRemoveTopLevel(id);
        doc->cmdInsertTopLevel(
            0, std::make_unique<SolidColorLayer>(Rgba8{1, 2, 3, 255}, Rect{0, 0, 50, 50}, "Fill"));
        PE_CHECK_EQ(scansFor(*doc), static_cast<std::uint64_t>(0));
    }

    {  // Off-canvas content does not change the count either. It used to: the version
        // check short-circuited on the first off-canvas layer, so how many times a save
        // scanned a document depended on its content. One scan per store, always.
        auto doc = Document::createBlank(Size{200, 150});
        asPixel(*doc, 0)->tiles().fillRect(Rect{-40, -40, 20, 20}, Rgba8{1, 2, 3, 255});
        for (int i = 0; i < 2; ++i) {
            auto extra = std::make_unique<PixelLayer>("L", BitDepth::U8);
            extra->tiles().fillRect(Rect{10, 10, 100, 100}, Rgba8{4, 5, 6, 255});
            doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(extra));
        }
        PE_CHECK_EQ(scansFor(*doc), static_cast<std::uint64_t>(3));
    }
}

PE_TEST(native_format_emits_the_same_rect_the_version_decision_used) {
    // The two consumers must agree. If the version decision saw one rectangle and the
    // record carried another, a file could declare v6 while containing an off-canvas rect,
    // and its own reader would then refuse it. That is the shape of #173, reached a
    // different way.
    //
    // Checked over the boundary between the two versions: content just inside the canvas
    // must stay v6, and content one pixel outside must go to v7 and round-trip.
    for (const bool offCanvas : {false, true}) {
        auto doc = Document::createBlank(Size{200, 150});
        auto* base = asPixel(*doc, 0);
        base->setName("");
        base->tiles().fillRect(offCanvas ? Rect{-1, 0, 20, 20} : Rect{0, 0, 20, 20},
                               Rgba8{1, 2, 3, 255});
        const std::vector<std::byte> blob = serializeDocument(*doc);
        PE_CHECK_EQ(static_cast<int>(blob[5]),
                    static_cast<int>(offCanvas ? std::byte{'7'} : std::byte{'6'}));

        const auto back = deserializeDocument(blob);
        PE_CHECK(back != nullptr);
        if (back == nullptr) continue;
        // The pixels landed where they were, which is only possible if the emitted rect
        // matched the one the version was chosen from.
        const auto* rl = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
        PE_CHECK(rl != nullptr);
        if (rl == nullptr) continue;
        PE_CHECK(rl->tiles().pixel(offCanvas ? -1 : 0, 0) == Rgba8{1, 2, 3, 255});
        PE_CHECK(serializeDocument(*back) == blob);
    }
}

namespace {

// The oracle for the gather cases below: TileStoreT::pixel, which is the per-pixel access
// path the writer used to use and no longer does. Comparing the round-tripped document
// against it checks the new run-based traversal against the old semantics directly,
// including for absent tiles and negative coordinates.
bool roundTripMatchesPerPixelOracle(const Document& doc, Rect probe) {
    const std::vector<std::byte> blob = serializeDocument(doc);
    const auto back = deserializeDocument(blob);
    if (back == nullptr) return false;
    const auto* src = dynamic_cast<const PixelLayer*>(doc.topLevelLayers()[0].get());
    const auto* dst = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
    if (src == nullptr || dst == nullptr) return false;
    for (int y = probe.top(); y < probe.bottom(); ++y) {
        for (int x = probe.left(); x < probe.right(); ++x) {
            if (!(dst->tiles().pixel(x, y) == src->tiles().pixel(x, y))) return false;
        }
    }
    return true;
}

// How many tile columns a horizontal span crosses, which is how many lookups one output
// row costs under the run-based gather.
int tileColumnsSpanned(int x, int width) {
    if (width <= 0) return 0;
    return floorDiv(x + width - 1, kTileSize) - floorDiv(x, kTileSize) + 1;
}

}  // namespace

PE_TEST(native_format_gathers_pixels_per_tile_run_not_per_pixel) {
    // #176. The writer used to read every pixel through store.pixel(x, y), a floorDiv pair
    // plus a map lookup, so a 24 MP layer cost 24 million lookups. A run never crosses a
    // tile boundary, so one lookup covers up to kTileSize samples.
    //
    // Asserted as a count, and as a SCALING property: the exact figure follows from the
    // geometry of the rect, so a regression to per-pixel access cannot pass by coincidence.
    struct Case {
        Rect content;
        const char* what;
    };
    const Case cases[] = {
        {Rect{0, 0, 4 * kTileSize, 3 * kTileSize}, "tile-aligned, several tiles each way"},
        {Rect{7, 9, 2 * kTileSize + 40, kTileSize + 5}, "starts and ends mid-tile"},
        {Rect{0, 0, 1, 1}, "one pixel"},
        {Rect{kTileSize - 1, 0, 2, 40}, "two pixels straddling a tile boundary"},
        {Rect{-kTileSize - 3, -kTileSize - 3, 2 * kTileSize, 40}, "negative origin"},
    };
    for (const Case& c : cases) {
        auto doc = Document::createBlank(Size{4 * kTileSize, 3 * kTileSize});
        auto* base = asPixel(*doc, 0);
        base->tiles().fillRect(c.content, Rgba8{11, 22, 33, 255});

        const std::uint64_t before = gatherTileLookupCount();
        (void)serializeDocument(*doc);
        const std::uint64_t lookups = gatherTileLookupCount() - before;

        // The content rect the writer emits is the tight bounds of what was painted, which
        // for a solid fill is exactly the rect above.
        const auto expected =
            static_cast<std::uint64_t>(c.content.height) *
            static_cast<std::uint64_t>(tileColumnsSpanned(c.content.x, c.content.width));
        PE_CHECK_EQ(lookups, expected);
        const auto pixels = static_cast<std::uint64_t>(c.content.width) *
                            static_cast<std::uint64_t>(c.content.height);
        if (c.content.width >= kTileSize) PE_CHECK(lookups * 8 <= pixels);
        // And where a run has room to amortize, it really is far below one per pixel, which
        // is the property that matters. A rect narrower than a tile can legitimately cost
        // one lookup per pixel (a 2-pixel-wide span across a boundary is two runs of one),
        // so the ratio is only meaningful once the rect is at least a tile wide.
    }
}

PE_TEST(native_format_gather_is_exact_at_tile_boundaries) {
    // The traversal splits every output row at tile edges, which is where an off-by-one
    // lives. Each case compares the round trip against TileStoreT::pixel, the per-pixel
    // path the writer abandoned, so the old semantics are the oracle rather than a
    // hand-written expectation.
    constexpr int T = kTileSize;
    const Rect fills[] = {
        {T - 1, T - 1, 1, 1},            // the last pixel of a tile
        {T, T, 1, 1},                    // the first pixel of the next
        {T + 1, T + 1, 1, 1},            // one past that
        {T - 1, 0, 2, 1},                // two pixels across the boundary
        {T - 1, 0, 2, T + 2},            // and down across a row boundary too
        {0, 0, T, T},                    // exactly one whole tile
        {0, 0, T + 1, T + 1},            // one pixel into the next tile each way
        {5, 5, 1, 3 * T},                // one pixel wide, three tiles tall
        {5, 5, 3 * T, 1},                // one pixel tall, three tiles wide
        {-1, -1, 2, 2},                  // straddling the origin: a negative tile coordinate
        {-T, -T, 1, 1},                  // the first pixel of the tile at (-1,-1)
        {-T - 1, -T - 1, 1, 1},          // the last pixel of the tile at (-2,-2)
        {-T - 1, -T - 1, T + 2, T + 2},  // spanning three tiles from a negative origin
    };
    for (const Rect fill : fills) {
        auto doc = Document::createBlank(Size{4 * T, 4 * T});
        auto* base = asPixel(*doc, 0);
        base->tiles().fillRect(fill, Rgba8{200, 100, 50, 255});
        // Probe a generous margin around the fill, so a run that emitted one pixel too many
        // or too few shows up as a changed neighbour rather than being missed.
        const Rect probe{fill.x - 2, fill.y - 2, fill.width + 4, fill.height + 4};
        PE_CHECK(roundTripMatchesPerPixelOracle(*doc, probe));
    }
}

PE_TEST(native_format_gather_keeps_absent_tiles_transparent) {
    // A missing tile is image semantics, not an absence of data: pixel() returns a
    // transparent sample for one, and the gather must emit exactly that. A content rect
    // spanning a hole is the case, because the rect is the union of the painted tiles and
    // the gap between them is never allocated.
    constexpr int T = kTileSize;
    auto doc = Document::createBlank(Size{6 * T, 2 * T});
    auto* base = asPixel(*doc, 0);
    base->tiles().fillRect(Rect{0, 0, T, T}, Rgba8{10, 20, 30, 255});
    base->tiles().fillRect(Rect{4 * T, 0, T, T}, Rgba8{40, 50, 60, 255});  // three tiles of gap
    PE_CHECK_EQ(base->tiles().tileCount(), static_cast<std::size_t>(2));

    const std::vector<std::byte> blob = serializeDocument(*doc);
    const auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    const auto* dst = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
    PE_REQUIRE(dst != nullptr);

    // The hole round-trips as transparent, and creates no tiles on either side.
    PE_CHECK(dst->tiles().pixel(2 * T + 5, 5) == Rgba8{});
    PE_CHECK(dst->tiles().pixel(0, 0) == Rgba8{10, 20, 30, 255});
    PE_CHECK(dst->tiles().pixel(4 * T, 0) == Rgba8{40, 50, 60, 255});
    PE_CHECK(roundTripMatchesPerPixelOracle(*doc, Rect{-4, -4, 6 * T + 8, 2 * T + 8}));
    // Serialization is a read: it must not have materialized the gap in the source either.
    PE_CHECK_EQ(base->tiles().tileCount(), static_cast<std::size_t>(2));
}

PE_TEST(native_format_gather_is_exact_for_every_pixel_depth) {
    // The gather is a template, so a mistake can be made once per instantiation. Each depth
    // is checked against its own store's per-pixel accessor across a tile boundary.
    constexpr int T = kTileSize;
    {
        auto doc = Document::createBlank(Size{2 * T, T}, ColorMode::RGB, BitDepth::U16, 72);
        auto* base = asPixel(*doc, 0);
        base->tiles16().fillRect(Rect{T - 3, 4, 6, 6}, Rgba16{4000, 5000, 6000, 65535});
        const auto back = deserializeDocument(serializeDocument(*doc));
        PE_REQUIRE(back != nullptr);
        const auto* dst = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
        PE_REQUIRE(dst != nullptr);
        for (int y = 0; y < 16; ++y) {
            for (int x = T - 6; x < T + 6; ++x) {
                PE_CHECK(dst->tiles16().pixel(x, y) == base->tiles16().pixel(x, y));
            }
        }
    }
    {
        auto doc = Document::createBlank(Size{2 * T, T}, ColorMode::RGB, BitDepth::F32, 72);
        auto* base = asPixel(*doc, 0);
        base->tilesF().fillRect(Rect{T - 3, 4, 6, 6}, Rgbaf{0.25f, 0.5f, 0.75f, 1.0f});
        const auto back = deserializeDocument(serializeDocument(*doc));
        PE_REQUIRE(back != nullptr);
        const auto* dst = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
        PE_REQUIRE(dst != nullptr);
        for (int y = 0; y < 16; ++y) {
            for (int x = T - 6; x < T + 6; ++x) {
                const Rgbaf a = dst->tilesF().pixel(x, y);
                const Rgbaf e = base->tilesF().pixel(x, y);
                PE_CHECK(a.r == e.r && a.g == e.g && a.b == e.b && a.a == e.a);
            }
        }
    }
}

PE_TEST(native_format_mask_gather_is_exact_across_tiles_and_holes) {
    // The mask block had the same per-pixel lookup and got the same treatment. An absent
    // mask tile reads as kOpaque, not as zero, so treating a hole as "no data" would
    // silently invert the meaning of the gap.
    constexpr int T = kTileSize;
    auto doc = Document::createBlank(Size{4 * T, 2 * T});
    auto* base = asPixel(*doc, 0);
    base->tiles().fillRect(Rect{0, 0, 4 * T, 2 * T}, Rgba8{9, 9, 9, 255});
    auto m = std::make_unique<Mask>();
    m->buffer().fillRect(Rect{T - 2, T - 2, 4, 4}, MaskBuffer::kClear);  // straddles a corner
    m->buffer().fillRect(Rect{3 * T, 5, 10, 10}, MaskBuffer::kClear);    // with a gap between
    base->setMask(std::move(m));

    const auto back = deserializeDocument(serializeDocument(*doc));
    PE_REQUIRE(back != nullptr);
    const Layer* rl = back->topLevelLayers()[0].get();
    PE_REQUIRE(rl != nullptr);
    PE_REQUIRE(rl->mask() != nullptr);
    // Counted rather than one assertion per pixel: a broken gather differs at tens of
    // thousands of coordinates, and 64,788 identical failure lines bury every other result
    // in the run. The first mismatch is reported with its coordinate, which is the part
    // that actually helps.
    int mismatches = 0;
    int firstX = 0;
    int firstY = 0;
    for (int y = -2; y < 2 * T + 2; ++y) {
        for (int x = -2; x < 4 * T + 2; ++x) {
            if (rl->mask()->buffer().value(x, y) != base->mask()->buffer().value(x, y)) {
                if (mismatches == 0) {
                    firstX = x;
                    firstY = y;
                }
                ++mismatches;
            }
        }
    }
    PE_CHECK_EQ(mismatches, 0);
    if (mismatches != 0) {
        std::printf("    first mask mismatch at (%d,%d), %d in total\n", firstX, firstY,
                    mismatches);
    }
}

#ifdef PHOTOEDIT_HAVE_ZLIB
namespace {

// The .pedoc written under the pre-#177 Z_DEFAULT_COMPRESSION policy, checked into the
// repository. It is a compatibility fixture: it must keep loading whatever level the writer
// is set to, and it is never regenerated to make a test pass.
//
// Guarded on zlib because its blocks are compressed, and a build without zlib cannot read a
// compressed .pedoc at all: readBlock returns false for a compressed block and says so.
// That is a pre-existing property of the no-optional-deps configuration, not something this
// slice changed.
std::vector<std::byte> readLegacyFixture() {
    std::ifstream f(std::string(PE_FIXTURE_DIR) + "/legacy-default-compression.pedoc",
                    std::ios::binary);
    if (!f) return {};
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

}  // namespace

PE_TEST(native_format_reads_a_file_written_under_the_previous_compression_policy) {
    // #177 changed the writer's DEFLATE level from zlib's default to Z_BEST_SPEED. The level
    // is not recorded in the file and a zlib stream is self-describing, so this should be a
    // non-event for readers. "Should be" is not evidence, and a format that quietly stopped
    // reading its own older files would be the worst possible outcome of a performance
    // change, so a file written under the old policy is committed and loaded here.
    const std::vector<std::byte> blob = readLegacyFixture();
    PE_REQUIRE(!blob.empty());
    PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'7'}));

    const auto doc = deserializeDocument(blob);
    PE_REQUIRE(doc != nullptr);
    PE_CHECK_EQ(doc->canvasSize().width, 300);
    PE_CHECK_EQ(doc->canvasSize().height, 200);
    PE_CHECK_EQ(doc->resolutionPpi(), 144);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(3));

    const auto* base = dynamic_cast<const PixelLayer*>(doc->topLevelLayers()[0].get());
    PE_REQUIRE(base != nullptr);
    PE_CHECK(base->name() == "Base");
    PE_CHECK(base->blendMode() == BlendMode::Multiply);
    PE_CHECK(base->opacity() > 0.74f && base->opacity() < 0.76f);
    // Pixels, including the off-canvas patch that made the file v7.
    PE_CHECK(base->tiles().pixel(10, 20) == Rgba8{10, 20, 30, 255});
    PE_CHECK(base->tiles().pixel(-40, -30) == Rgba8{7, 7, 7, 255});
    // The mask, with its flag.
    PE_REQUIRE(base->mask() != nullptr);
    PE_CHECK(base->mask()->inverted());
    PE_CHECK_EQ(static_cast<int>(base->mask()->buffer().value(20, 20)),
                static_cast<int>(MaskBuffer::kClear));
    // The nested group and the off-canvas solid fill.
    const auto* group = dynamic_cast<const GroupLayer*>(doc->topLevelLayers()[1].get());
    PE_REQUIRE(group != nullptr);
    PE_CHECK_EQ(group->childCount(), static_cast<std::size_t>(1));
    const auto* solid = dynamic_cast<const SolidColorLayer*>(doc->topLevelLayers()[2].get());
    PE_REQUIRE(solid != nullptr);
    PE_CHECK(solid->bounds() == Rect{-10, -10, 90, 90});
}

PE_TEST(native_format_compression_policy_is_the_chosen_one_not_zlibs_default) {
    // The policy is a decision, so it is asserted rather than left to whatever compress2
    // defaults to. The level is not recorded in the file, so this infers it from the only
    // observable consequence: how large the output is for a payload whose compressed size
    // differs sharply between levels.
    //
    // Bounds rather than an exact size, because zlib's output is not contractually stable
    // across versions. Measured on this exact payload with this zlib:
    //
    //   level 0   1.00x   (stored raw: deflate made it bigger, so writeBlock falls back)
    //   level 1  26.75x
    //   level 3  27.44x
    //   level 6  54.79x
    //   level 9  55.68x
    //
    // So the window below admits level 1 with wide margins and excludes 0, 6 and 9. It does
    // NOT distinguish 1 from 3, which is fine: the values this guards against are the ones
    // something could drift back to by accident, and those are 6 (zlib's default, which is
    // what this slice replaced) and 0 (the other obvious constant). Nothing lands on 3 by
    // accident.
    auto doc = Document::createBlank(Size{1200, 900});
    auto* base = asPixel(*doc, 0);
    for (int y = 0; y < 900; ++y) {
        for (int x = 0; x < 1200; ++x) {
            const auto v = static_cast<std::uint8_t>(((x / 3) ^ (y / 5)) & 0xFF);
            base->tiles().setPixel(x, y, Rgba8{v, static_cast<std::uint8_t>(v / 2), 64, 255});
        }
    }
    const std::vector<std::byte> blob = serializeDocument(*doc);
    const double raw = 1200.0 * 900.0 * 4.0;
    const double ratio = raw / static_cast<double>(blob.size());

    PE_CHECK(ratio > 5.0);   // really compressed, so not level 0 and not a raw fallback
    PE_CHECK(ratio < 40.0);  // and not squeezed the way 6 or 9 would

    const auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    const auto* rl = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
    PE_REQUIRE(rl != nullptr);
    PE_CHECK(rl->tiles().pixel(700, 400) == base->tiles().pixel(700, 400));
}
#endif  // PHOTOEDIT_HAVE_ZLIB

PE_TEST(native_format_payload_survives_the_compression_policy_unchanged) {
    // Compressed bytes differ between levels, so the byte-equality oracle used for #174 and
    // #176 is the wrong assertion here. What must not change is the DECOMPRESSED payload:
    // the same document in, the same document out. Unguarded, because a build without zlib
    // stores every block raw and must round-trip identically too.
    constexpr int T = kTileSize;
    for (const BitDepth depth : {BitDepth::U8, BitDepth::U16, BitDepth::F32}) {
        auto doc = Document::createBlank(Size{2 * T, T}, ColorMode::RGB, depth, 96);
        auto* base = asPixel(*doc, 0);
        const auto paint = [depth, base](Rect r, int seed) {
            switch (depth) {
                case BitDepth::U16:
                    base->tiles16().fillRect(
                        r, Rgba16{static_cast<std::uint16_t>(seed * 1000), 2000, 3000, 65535});
                    break;
                case BitDepth::F32:
                    base->tilesF().fillRect(
                        r, Rgbaf{static_cast<float>(seed) / 10.0f, 0.25f, 0.5f, 1.0f});
                    break;
                case BitDepth::U8:
                default:
                    base->tiles().fillRect(
                        r, Rgba8{static_cast<std::uint8_t>(seed * 20), 40, 60, 255});
                    break;
            }
        };
        paint(Rect{0, 0, 2 * T, T}, 1);
        paint(Rect{-30, -30, 25, 25}, 5);  // off-canvas, so the file is v7
        auto m = std::make_unique<Mask>();
        m->buffer().fillRect(Rect{T - 5, 5, 20, 20}, MaskBuffer::kClear);
        base->setMask(std::move(m));

        const std::vector<std::byte> blob = serializeDocument(*doc);
        PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'7'}));
        const auto back = deserializeDocument(blob);
        PE_CHECK(back != nullptr);
        if (back == nullptr) continue;
        const auto* rl = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
        PE_CHECK(rl != nullptr);
        if (rl == nullptr) continue;

        int mismatches = 0;
        for (int y = -32; y < T + 2; ++y) {
            for (int x = -32; x < 2 * T + 2; ++x) {
                switch (depth) {
                    case BitDepth::U16:
                        if (!(rl->tiles16().pixel(x, y) == base->tiles16().pixel(x, y))) {
                            ++mismatches;
                        }
                        break;
                    case BitDepth::F32: {
                        const Rgbaf a = rl->tilesF().pixel(x, y);
                        const Rgbaf e = base->tilesF().pixel(x, y);
                        if (a.r != e.r || a.g != e.g || a.b != e.b || a.a != e.a) ++mismatches;
                        break;
                    }
                    case BitDepth::U8:
                    default:
                        if (!(rl->tiles().pixel(x, y) == base->tiles().pixel(x, y))) ++mismatches;
                        break;
                }
                if (rl->mask() != nullptr && base->mask() != nullptr &&
                    rl->mask()->buffer().value(x, y) != base->mask()->buffer().value(x, y)) {
                    ++mismatches;
                }
            }
        }
        PE_CHECK_EQ(mismatches, 0);
        // And re-serializing the reloaded document reproduces the same bytes, so the writer
        // stays deterministic under the new policy.
        PE_CHECK(serializeDocument(*back) == blob);
    }
}

PE_TEST(native_format_stores_a_block_raw_rather_than_failing_when_it_cannot_shrink_it) {
    // The failure contract, established rather than assumed. zlibDeflate returns empty on
    // any zlib error, and incompressible data legitimately deflates larger than it started;
    // writeBlock stores the block raw in both cases. Raw is a first-class encoding of this
    // format, so the save succeeds and produces a correct, larger file. Refusing to save
    // because a compressor was unhappy would lose the user's work.
    auto doc = Document::createBlank(Size{600, 400});
    auto* base = asPixel(*doc, 0);
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            const auto r = static_cast<std::uint32_t>(x) * 1103515245u +
                           static_cast<std::uint32_t>(y) * 12345u;
            base->tiles().setPixel(
                x, y,
                Rgba8{static_cast<std::uint8_t>(r >> 3), static_cast<std::uint8_t>(r >> 11),
                      static_cast<std::uint8_t>(r >> 19), static_cast<std::uint8_t>(r >> 25)});
        }
    }
    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(!blob.empty());
    // Bigger than the pixels it holds, which is only possible if a block went in raw.
    PE_CHECK(blob.size() > static_cast<std::size_t>(600 * 400 * 4));

    const auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    const auto* rl = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
    PE_REQUIRE(rl != nullptr);
    int mismatches = 0;
    for (int y = 0; y < 400; y += 7) {
        for (int x = 0; x < 600; x += 5) {
            if (!(rl->tiles().pixel(x, y) == base->tiles().pixel(x, y))) ++mismatches;
        }
    }
    PE_CHECK_EQ(mismatches, 0);  // raw storage is lossless
}

PE_TEST(native_format_round_trips_a_layer_past_the_old_dense_area_cap) {
    // The reader used to refuse any dense rect over 64 MP, and nothing stopped the writer
    // emitting one, so PhotoEdit wrote files it could not read back. Not an exotic case: a
    // 10000x10000 canvas with a filled layer wrote 400 MB and would not reopen, and so did
    // an 8000x8000 photo (exactly the decoders' own cap) the moment one brush dab landed a
    // pixel past the corner. The save reported success both times. Memory is bounded by
    // readBlock's aggregate budget, which is checked before every allocation and charges the
    // resident tiled footprint, so the area cap was protecting nothing the budget did not
    // already cover more accurately. See native_format_budget_rejects_excess_allocation.
    //
    // 8000x8001 is the cheapest document that clears the old cap (64,008,000 px, 8,000 over)
    // and the content is two pixels, so the tile store stays tiny; the dense gather is
    // inherently ~256 MB and there is no way to exercise this boundary for less.
    auto doc = Document::createBlank(Size{8000, 8001});
    PE_REQUIRE(doc != nullptr);
    auto* base = asPixel(*doc, 0);
    PE_REQUIRE(base != nullptr);
    base->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
    base->tiles().setPixel(7999, 8000, Rgba8{4, 5, 6, 255});

    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_CHECK(!blob.empty());
    const auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    const auto* rl = dynamic_cast<const PixelLayer*>(back->topLevelLayers()[0].get());
    PE_REQUIRE(rl != nullptr);
    PE_CHECK(rl->tiles().pixel(0, 0) == (Rgba8{1, 2, 3, 255}));
    PE_CHECK(rl->tiles().pixel(7999, 8000) == (Rgba8{4, 5, 6, 255}));
    PE_CHECK(rl->tiles().pixel(4000, 4000) == (Rgba8{0, 0, 0, 0}));
}

PE_TEST(native_format_refuses_to_write_a_rect_it_could_not_read_back) {
    // The producer half of the persisted-geometry contract. readContentRect bounds every
    // rect to the engine's coordinate range; the writer had no such rule, and two ordinary
    // paths walk past it. A brush dab is bounded only at +/-2^26, and SolidColorLayer's
    // bounds are not bounded at all, so either could put content 400,000 px out. Both wrote
    // a file that reported success and then would not reopen: 17 MB for the pixels, 102
    // bytes for the fill. Refusing while the work is still in memory is the whole point, so
    // the assertion is that NOTHING was produced.
    const int far = kMaxCanvasDimension + 100'000;
    {
        auto doc = Document::createBlank(Size{512, 512});
        PE_REQUIRE(doc != nullptr);
        auto* base = asPixel(*doc, 0);
        PE_REQUIRE(base != nullptr);
        base->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
        // In range on its own, so this is not passing merely because the document is odd.
        PE_CHECK(!serializeDocument(*doc).empty());
        base->tiles().setPixel(far, 10, Rgba8{4, 5, 6, 255});
        PE_CHECK(serializeDocument(*doc).empty());
    }
    {
        auto doc = Document::createBlank(Size{512, 512});
        PE_REQUIRE(doc != nullptr);
        doc->cmdInsertTopLevel(
            0, std::make_unique<SolidColorLayer>(Rgba8{9, 9, 9, 255}, Rect{0, 0, 50, 50}, "Fill"));
        PE_CHECK(!serializeDocument(*doc).empty());
        auto* solid = const_cast<SolidColorLayer*>(firstSolid(*doc));
        PE_REQUIRE(solid != nullptr);
        solid->setBounds(Rect{far, 0, 50, 50});
        PE_CHECK(serializeDocument(*doc).empty());
    }
    {
        // A mask reaching out of range is refused for the same reason, through the same rect.
        auto doc = Document::createBlank(Size{512, 512});
        PE_REQUIRE(doc != nullptr);
        auto* base = asPixel(*doc, 0);
        PE_REQUIRE(base != nullptr);
        base->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
        auto m = std::make_unique<Mask>();
        m->buffer().fillRect(Rect{far, 0, 8, 8}, MaskBuffer::kClear);
        base->setMask(std::move(m));
        PE_CHECK(serializeDocument(*doc).empty());
    }
}

// --- fill opacity, clipping and locks (#141) ----------------------------------------------

PE_TEST(native_format_round_trips_fill_opacity_clipping_and_locks) {
    // The writer emitted kind, visible, opacity, blend mode, the active flag and the name,
    // and nothing else. fillOpacity and clipped both change the composited picture, so a
    // document round-tripped through its own native format came back rendering differently
    // from the one that was saved, and said nothing about it.
    auto doc = Document::createBlank(Size{32, 32});
    auto* base = asPixel(*doc, 0);
    base->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{10, 20, 30, 255});
    base->setLocks(
        LayerLocks{.transparency = true, .pixels = false, .position = true, .all = false});

    auto over = std::make_unique<PixelLayer>("Over");
    over->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{200, 0, 0, 255});
    over->setFillOpacity(0.25f);
    over->setClipped(true);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(over));

    // A group, so the extended fields are exercised through the nested record path too.
    auto group = std::make_unique<GroupLayer>("G");
    auto inner = std::make_unique<PixelLayer>("Inner");
    inner->tiles().fillRect(Rect{4, 4, 8, 8}, Rgba8{0, 200, 0, 255});
    inner->setFillOpacity(0.5f);
    inner->setLocks(
        LayerLocks{.transparency = false, .pixels = true, .position = false, .all = true});
    group->addChild(std::move(inner));
    group->setClipped(true);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_REQUIRE(blob.size() > 6);
    // The version is a property of the CONTENT: this document needs v8, so it says so, and
    // a build that predates v8 refuses it rather than dropping what it cannot represent.
    PE_CHECK_EQ(static_cast<int>(blob[5]), static_cast<int>(std::byte{'8'}));

    auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr);
    PE_REQUIRE(back->topLevelCount() == static_cast<std::size_t>(3));

    const Layer* rbase = back->topLevelLayers()[0].get();
    PE_CHECK_NEAR(rbase->fillOpacity(), 1.0f);
    PE_CHECK(!rbase->clipped());
    PE_CHECK(rbase->locks() ==
             LayerLocks{.transparency = true, .pixels = false, .position = true, .all = false});

    const Layer* rover = back->topLevelLayers()[1].get();
    PE_CHECK_NEAR(rover->fillOpacity(), 0.25f);
    PE_CHECK(rover->clipped());
    PE_CHECK(rover->locks() == LayerLocks{});

    const Layer* rgroup = back->topLevelLayers()[2].get();
    PE_CHECK(rgroup->clipped());
    const auto* g = dynamic_cast<const GroupLayer*>(rgroup);
    PE_REQUIRE(g != nullptr && g->childCount() == static_cast<std::size_t>(1));
    const Layer* rinner = g->children()[0].get();
    PE_CHECK_NEAR(rinner->fillOpacity(), 0.5f);
    PE_CHECK(rinner->locks() ==
             LayerLocks{.transparency = false, .pixels = true, .position = false, .all = true});
}

PE_TEST(native_format_stays_on_the_old_version_when_no_layer_uses_the_new_properties) {
    // Same rule the off-canvas bump follows: claiming the newer version on every save would
    // stop older builds reading ordinary files, for a document that uses none of it.
    auto doc = Document::createBlank(Size{32, 32});
    asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{1, 2, 3, 255});
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*doc)[5]), static_cast<int>(std::byte{'6'}));

    // Setting a property back to its default keeps the file on the old version: the test is
    // the VALUE, not whether a setter was called.
    asPixel(*doc, 0)->setFillOpacity(1.0f);
    asPixel(*doc, 0)->setClipped(false);
    asPixel(*doc, 0)->setLocks(LayerLocks{});
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*doc)[5]), static_cast<int>(std::byte{'6'}));

    // Each of the three on its own is enough to need v8.
    asPixel(*doc, 0)->setFillOpacity(0.5f);
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*doc)[5]), static_cast<int>(std::byte{'8'}));
    asPixel(*doc, 0)->setFillOpacity(1.0f);
    asPixel(*doc, 0)->setClipped(true);
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*doc)[5]), static_cast<int>(std::byte{'8'}));
    asPixel(*doc, 0)->setClipped(false);
    asPixel(*doc, 0)->setLocks(LayerLocks{.transparency = true});
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*doc)[5]), static_cast<int>(std::byte{'8'}));

    // And off-canvas content alone still produces v7, not v8: the two reasons are separate.
    asPixel(*doc, 0)->setLocks(LayerLocks{});
    asPixel(*doc, 0)->tiles().setPixel(-1, -1, Rgba8{4, 5, 6, 255});
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*doc)[5]), static_cast<int>(std::byte{'7'}));

    // A property on a NESTED layer needs v8 just as much: the record layout is per file, so
    // a scan that stopped at the top level would emit a v6 header and then v8 records.
    auto nested = Document::createBlank(Size{32, 32});
    auto group = std::make_unique<GroupLayer>("G");
    auto inner = std::make_unique<PixelLayer>("Inner");
    inner->tiles().fillRect(Rect{2, 2, 4, 4}, Rgba8{9, 9, 9, 255});
    group->addChild(std::move(inner));
    nested->cmdInsertTopLevel(nested->topLevelCount(), std::move(group));
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*nested)[5]), static_cast<int>(std::byte{'6'}));

    auto* g = dynamic_cast<GroupLayer*>(
        const_cast<Layer*>(nested->topLevelLayers()[1].get()));  // NOLINT: test convenience
    PE_REQUIRE(g != nullptr && g->childCount() == static_cast<std::size_t>(1));
    const_cast<Layer*>(g->children()[0].get())->setFillOpacity(0.5f);  // NOLINT
    PE_CHECK_EQ(static_cast<int>(serializeDocument(*nested)[5]), static_cast<int>(std::byte{'8'}));
    auto backNested = deserializeDocument(serializeDocument(*nested));
    PE_REQUIRE(backNested != nullptr);
    const auto* bg = dynamic_cast<const GroupLayer*>(backNested->topLevelLayers()[1].get());
    PE_REQUIRE(bg != nullptr && bg->childCount() == static_cast<std::size_t>(1));
    PE_CHECK_NEAR(bg->children()[0]->fillOpacity(), 0.5f);
}

PE_TEST(native_format_gives_a_pre_v8_file_the_model_defaults) {
    // A v6 file carries none of the three, and must read back with exactly the values it
    // rendered with rather than whatever happened to be in memory.
    auto doc = Document::createBlank(Size{32, 32});
    asPixel(*doc, 0)->tiles().fillRect(Rect{0, 0, 32, 32}, Rgba8{1, 2, 3, 255});
    const std::vector<std::byte> blob = serializeDocument(*doc);
    PE_REQUIRE(blob.size() > 6);
    PE_REQUIRE(static_cast<int>(blob[5]) == static_cast<int>(std::byte{'6'}));

    auto back = deserializeDocument(blob);
    PE_REQUIRE(back != nullptr && back->topLevelCount() == static_cast<std::size_t>(1));
    const Layer* rl = back->topLevelLayers()[0].get();
    PE_CHECK_NEAR(rl->fillOpacity(), 1.0f);
    PE_CHECK(!rl->clipped());
    PE_CHECK(rl->locks() == LayerLocks{});
}

PE_TEST(native_format_rejects_a_bad_clip_flag) {
    // The clip flag is a boolean. Any other byte means the stream is not the record it
    // claims to be, and reading on would misinterpret every field after it.
    auto doc = Document::createBlank(Size{4, 4});
    asPixel(*doc, 0)->setName("");  // no name bytes, so the offsets below are stable
    asPixel(*doc, 0)->setClipped(true);
    std::vector<std::byte> blob = serializeDocument(*doc);
    PE_REQUIRE(!blob.empty());

    // header: 6 magic + 4 version + 4 w + 4 h + 1 mode + 1 depth + 4 ppi + 4 topCount = 28
    // record: 1 kind + 1 visible + 4 opacity + 1 blend + 1 active + 4 nameLen = 12, then
    // 4 fillOpacity, then the clip byte.
    constexpr std::size_t kClipByte = 28 + 12 + 4;
    PE_REQUIRE(blob.size() > kClipByte);
    PE_REQUIRE(static_cast<int>(blob[kClipByte]) == 1);  // the flag we set, where we expect it
    blob[kClipByte] = std::byte{2};
    PE_CHECK(deserializeDocument(blob) == nullptr);
}
