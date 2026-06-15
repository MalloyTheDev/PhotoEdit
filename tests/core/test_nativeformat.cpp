#include "pe/core/Document.hpp"
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

    // Layout: header 32 + visible(1)+opacity(4)+blend(1)+nameLen(4)=10 -> cx@42, cy@46.
    PE_CHECK(blob.size() > 50);
    blob[46] = std::byte{0xF0};
    blob[47] = std::byte{0xFF};
    blob[48] = std::byte{0xFF};
    blob[49] = std::byte{0x7F};                      // cy ~= 2.1e9, far outside the 4x4 canvas
    PE_CHECK(deserializeDocument(blob) == nullptr);  // rejected, no UB / over-allocation
}
