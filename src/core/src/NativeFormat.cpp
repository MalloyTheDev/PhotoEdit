#include "pe/core/NativeFormat.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace pe {

namespace {

constexpr char kMagic[6] = {'P', 'E', 'D', 'O', 'C', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::int64_t kMaxLayerPixels = 64'000'000;  // per-layer content cap
constexpr std::uint32_t kMaxLayers = 100'000;
constexpr std::uint32_t kMaxNameBytes = 65'536;

// --- little-endian byte writer ---
class Writer {
public:
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    void u8(std::uint8_t v) { bytes(&v, 1); }
    void u32(std::uint32_t v) { bytes(&v, 4); }
    void i32(std::int32_t v) { bytes(&v, 4); }
    void f32(float v) { bytes(&v, 4); }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

// --- little-endian bounds-checked byte reader ---
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool atEnd() const noexcept { return pos_ == data_.size(); }

    bool read(void* dst, std::size_t n) {
        if (!ok_ || pos_ + n > data_.size()) {
            ok_ = false;
            return false;
        }
        std::memcpy(dst, data_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    std::uint8_t u8() {
        std::uint8_t v = 0;
        read(&v, 1);
        return v;
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        read(&v, 4);
        return v;
    }
    std::int32_t i32() {
        std::int32_t v = 0;
        read(&v, 4);
        return v;
    }
    float f32() {
        float v = 0;
        read(&v, 4);
        return v;
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace

std::vector<std::byte> serializeDocument(const Document& doc) {
    Writer w;
    w.bytes(kMagic, sizeof(kMagic));
    w.u32(kVersion);

    const Size size = doc.canvasSize();
    w.i32(size.width);
    w.i32(size.height);
    w.u8(static_cast<std::uint8_t>(doc.colorMode()));
    w.u8(static_cast<std::uint8_t>(doc.bitDepth()));
    w.i32(doc.resolutionPpi());

    // Collect the top-level pixel layers (others are skipped in v1).
    std::vector<const PixelLayer*> layers;
    for (const auto& layer : doc.topLevelLayers()) {
        if (const auto* pl = dynamic_cast<const PixelLayer*>(layer.get())) layers.push_back(pl);
    }

    // The active layer's index among the serialized layers, or -1.
    std::int32_t activeIndex = -1;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i]->id() == doc.activeLayer()) activeIndex = static_cast<std::int32_t>(i);
    }
    w.i32(activeIndex);
    w.u32(static_cast<std::uint32_t>(layers.size()));

    for (const PixelLayer* pl : layers) {
        w.u8(pl->visible() ? 1 : 0);
        w.f32(pl->opacity());
        w.u8(static_cast<std::uint8_t>(pl->blendMode()));
        const std::string& name = pl->name();
        w.u32(static_cast<std::uint32_t>(name.size()));
        w.bytes(name.data(), name.size());

        // contentBounds() is tile-aligned (256px granular); clip to the canvas so the
        // blob is canvas-proportional, not padded to whole tiles. (Off-canvas layer
        // content is not preserved in v1.)
        const Rect b = pl->tiles().contentBounds().intersected(doc.canvasBounds());
        w.i32(b.x);
        w.i32(b.y);
        w.i32(b.width);
        w.i32(b.height);
        for (int y = b.y; y < b.y + b.height; ++y) {
            for (int x = b.x; x < b.x + b.width; ++x) {
                const Rgba8 px = pl->tiles().pixel(x, y);
                w.bytes(&px, sizeof(px));
            }
        }
    }
    return w.take();
}

std::unique_ptr<Document> deserializeDocument(std::span<const std::byte> data) {
    Reader r(data);

    char magic[6] = {};
    if (!r.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return nullptr;
    }
    if (r.u32() != kVersion) return nullptr;

    const std::int32_t canvasW = r.i32();
    const std::int32_t canvasH = r.i32();
    const std::uint8_t modeRaw = r.u8();
    const std::uint8_t depthRaw = r.u8();
    const std::int32_t ppi = r.i32();
    if (!r.ok()) return nullptr;

    // Validate the enum codes before trusting them.
    if (modeRaw > static_cast<std::uint8_t>(ColorMode::Bitmap)) return nullptr;
    if (depthRaw != 8 && depthRaw != 16 && depthRaw != 32) return nullptr;

    auto doc = Document::createBlank(Size{canvasW, canvasH}, static_cast<ColorMode>(modeRaw),
                                     static_cast<BitDepth>(depthRaw), ppi);
    if (doc == nullptr) return nullptr;  // createBlank rejects bad size/ppi

    const std::int32_t activeIndex = r.i32();
    const std::uint32_t layerCount = r.u32();
    if (!r.ok() || layerCount > kMaxLayers) return nullptr;

    // Drop the default layer(s) createBlank seeded; we rebuild the stack from the file.
    std::vector<LayerId> seeded;
    for (const auto& layer : doc->topLevelLayers()) seeded.push_back(layer->id());
    for (LayerId id : seeded) (void)doc->cmdRemoveTopLevel(id);

    std::vector<LayerId> newIds;
    for (std::uint32_t i = 0; i < layerCount; ++i) {
        const bool visible = r.u8() != 0;
        const float opacity = r.f32();
        const std::uint8_t blendRaw = r.u8();
        const std::uint32_t nameLen = r.u32();
        if (!r.ok() || blendRaw >= static_cast<std::uint8_t>(BlendMode::Count) ||
            nameLen > kMaxNameBytes) {
            return nullptr;
        }
        std::string name(nameLen, '\0');
        if (nameLen > 0 && !r.read(name.data(), nameLen)) return nullptr;

        const std::int32_t cx = r.i32();
        const std::int32_t cy = r.i32();
        const std::int32_t cw = r.i32();
        const std::int32_t ch = r.i32();
        // The content rect must lie within the canvas (the writer clips it there).
        // Validating in int64 keeps cx+cw / cy+ch from overflowing the int loop bounds
        // and confines tile allocation to the canvas — rejecting a hostile file that
        // sets an extreme origin or a thin region spanning a huge tile span.
        if (!r.ok() || cw < 0 || ch < 0 || cx < 0 || cy < 0 ||
            static_cast<std::int64_t>(cx) + cw > canvasW ||
            static_cast<std::int64_t>(cy) + ch > canvasH ||
            static_cast<std::int64_t>(cw) * ch > kMaxLayerPixels) {
            return nullptr;
        }

        auto layer = std::make_unique<PixelLayer>(std::move(name), static_cast<BitDepth>(depthRaw));
        layer->setVisible(visible);
        layer->setOpacity(opacity);
        layer->setBlendMode(static_cast<BlendMode>(blendRaw));

        TileStore& store = layer->tiles();
        for (int y = cy; y < cy + ch; ++y) {
            for (int x = cx; x < cx + cw; ++x) {
                Rgba8 px{};
                if (!r.read(&px, sizeof(px))) return nullptr;
                store.setPixel(x, y, px);
            }
        }

        newIds.push_back(layer->id());
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(layer));
    }

    if (activeIndex >= 0 && static_cast<std::size_t>(activeIndex) < newIds.size()) {
        doc->setActiveLayer(newIds[static_cast<std::size_t>(activeIndex)]);
    }
    return doc;
}

}  // namespace pe
