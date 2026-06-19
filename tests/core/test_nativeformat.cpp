#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/NativeFormat.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <memory>
#include <vector>

using namespace pe;

namespace {
PixelLayer* asPixel(Document& doc, std::size_t topIndex) {
    return dynamic_cast<PixelLayer*>(
        const_cast<Layer*>(doc.topLevelLayers()[topIndex].get()));  // NOLINT: test convenience
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));
    const auto* s = dynamic_cast<const SolidColorLayer*>(loaded->topLevelLayers()[1].get());
    PE_CHECK(s != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
    PE_CHECK_EQ(loaded->topLevelCount(), static_cast<std::size_t>(2));

    const auto& tops = loaded->topLevelLayers();
    auto* lgrp = dynamic_cast<GroupLayer*>(const_cast<Layer*>(tops[1].get()));
    PE_CHECK(lgrp != nullptr);
    PE_CHECK_EQ(lgrp->name(), std::string("Grp"));
    PE_CHECK_NEAR(lgrp->opacity(), 0.7f);
    PE_CHECK_EQ(lgrp->isolated(), false);
    PE_CHECK_EQ(lgrp->childCount(), static_cast<std::size_t>(1));

    auto* linner = dynamic_cast<PixelLayer*>(const_cast<Layer*>(lgrp->children()[0].get()));
    PE_CHECK(linner != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
    auto* lbase = asPixel(*loaded, 0);
    const Mask* lm = lbase->mask();
    PE_CHECK(lm != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
    PE_CHECK(loaded != nullptr);
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
