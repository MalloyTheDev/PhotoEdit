#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/NativeFormat.hpp"
#include "pe/core/PixelLayer.hpp"
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
