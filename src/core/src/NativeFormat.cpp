#include "pe/core/NativeFormat.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace pe {

namespace {

constexpr char kMagic[6] = {'P', 'E', 'D', 'O', 'C', '2'};
constexpr std::uint32_t kVersion = 2;
constexpr std::int64_t kMaxLayerPixels = 64'000'000;  // per-layer content cap
constexpr std::uint32_t kMaxLayers = 100'000;
constexpr std::uint32_t kMaxNameBytes = 65'536;
constexpr int kMaxGroupDepth = 256;  // cap nesting so a hostile file can't blow the stack

// Layer kind discriminators written to the stream.
constexpr std::uint8_t kKindPixel = 0;
constexpr std::uint8_t kKindGroup = 1;

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

// Whether the format serializes this layer (Pixel and Group in v2; adjustment/fill/
// text layers and embedded profiles round-trip later).
[[nodiscard]] bool serializable(const Layer& layer) {
    return layer.kind() == LayerKind::Group || dynamic_cast<const PixelLayer*>(&layer) != nullptr;
}

void writeLayer(Writer& w, const Layer& layer, Rect canvasBounds, LayerId active) {
    const bool isGroup = layer.kind() == LayerKind::Group;
    w.u8(isGroup ? kKindGroup : kKindPixel);
    w.u8(layer.visible() ? 1 : 0);
    w.f32(layer.opacity());
    w.u8(static_cast<std::uint8_t>(layer.blendMode()));
    w.u8(layer.id() == active ? 1 : 0);
    const std::string& name = layer.name();
    w.u32(static_cast<std::uint32_t>(name.size()));
    w.bytes(name.data(), name.size());

    if (isGroup) {
        const auto& group = static_cast<const GroupLayer&>(layer);
        w.u8(group.isolated() ? 1 : 0);
        std::vector<const Layer*> kids;
        for (const auto& child : group.children()) {
            if (serializable(*child)) kids.push_back(child.get());
        }
        w.u32(static_cast<std::uint32_t>(kids.size()));
        for (const Layer* child : kids) writeLayer(w, *child, canvasBounds, active);
        return;
    }

    // Pixel layer: content clipped to the canvas (contentBounds() is tile-aligned).
    const auto& pl = static_cast<const PixelLayer&>(layer);
    const Rect b = pl.tiles().contentBounds().intersected(canvasBounds);
    w.i32(b.x);
    w.i32(b.y);
    w.i32(b.width);
    w.i32(b.height);
    for (int y = b.y; y < b.y + b.height; ++y) {
        for (int x = b.x; x < b.x + b.width; ++x) {
            const Rgba8 px = pl.tiles().pixel(x, y);
            w.bytes(&px, sizeof(px));
        }
    }
}

// Reads one layer (recursively for groups). Returns nullptr on any inconsistency.
// Sets activeId/haveActive if a record carries the active flag.
std::unique_ptr<Layer> readLayer(Reader& r, int canvasW, int canvasH, BitDepth depth,
                                 LayerId& activeId, bool& haveActive, int depthGuard) {
    if (depthGuard > kMaxGroupDepth) return nullptr;

    const std::uint8_t kind = r.u8();
    const bool visible = r.u8() != 0;
    const float opacity = r.f32();
    const std::uint8_t blendRaw = r.u8();
    const std::uint8_t activeFlag = r.u8();
    const std::uint32_t nameLen = r.u32();
    if (!r.ok() || kind > kKindGroup || blendRaw >= static_cast<std::uint8_t>(BlendMode::Count) ||
        nameLen > kMaxNameBytes) {
        return nullptr;
    }
    std::string name(nameLen, '\0');
    if (nameLen > 0 && !r.read(name.data(), nameLen)) return nullptr;

    std::unique_ptr<Layer> layer;
    if (kind == kKindGroup) {
        auto group = std::make_unique<GroupLayer>(std::move(name));
        const std::uint8_t isolated = r.u8();
        const std::uint32_t childCount = r.u32();
        if (!r.ok() || childCount > kMaxLayers) return nullptr;
        group->setIsolated(isolated != 0);
        for (std::uint32_t i = 0; i < childCount; ++i) {
            auto child =
                readLayer(r, canvasW, canvasH, depth, activeId, haveActive, depthGuard + 1);
            if (child == nullptr) return nullptr;
            group->addChild(std::move(child));
        }
        layer = std::move(group);
    } else {
        auto pl = std::make_unique<PixelLayer>(std::move(name), depth);
        const std::int32_t cx = r.i32();
        const std::int32_t cy = r.i32();
        const std::int32_t cw = r.i32();
        const std::int32_t ch = r.i32();
        // Content must lie within the canvas (the writer clips to it); int64 math keeps
        // cx+cw / cy+ch from overflowing and confines the loop (and tile allocation).
        if (!r.ok() || cw < 0 || ch < 0 || cx < 0 || cy < 0 ||
            static_cast<std::int64_t>(cx) + cw > canvasW ||
            static_cast<std::int64_t>(cy) + ch > canvasH ||
            static_cast<std::int64_t>(cw) * ch > kMaxLayerPixels) {
            return nullptr;
        }
        TileStore& store = pl->tiles();
        for (int y = cy; y < cy + ch; ++y) {
            for (int x = cx; x < cx + cw; ++x) {
                Rgba8 px{};
                if (!r.read(&px, sizeof(px))) return nullptr;
                store.setPixel(x, y, px);
            }
        }
        layer = std::move(pl);
    }

    layer->setVisible(visible);
    layer->setOpacity(opacity);
    layer->setBlendMode(static_cast<BlendMode>(blendRaw));
    if (activeFlag != 0) {
        activeId = layer->id();
        haveActive = true;
    }
    return layer;
}

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

    std::vector<const Layer*> tops;
    for (const auto& layer : doc.topLevelLayers()) {
        if (serializable(*layer)) tops.push_back(layer.get());
    }
    w.u32(static_cast<std::uint32_t>(tops.size()));
    const Rect canvasBounds = doc.canvasBounds();
    for (const Layer* layer : tops) writeLayer(w, *layer, canvasBounds, doc.activeLayer());
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
    if (modeRaw > static_cast<std::uint8_t>(ColorMode::Bitmap)) return nullptr;
    if (depthRaw != 8 && depthRaw != 16 && depthRaw != 32) return nullptr;

    auto doc = Document::createBlank(Size{canvasW, canvasH}, static_cast<ColorMode>(modeRaw),
                                     static_cast<BitDepth>(depthRaw), ppi);
    if (doc == nullptr) return nullptr;

    const std::uint32_t topCount = r.u32();
    if (!r.ok() || topCount > kMaxLayers) return nullptr;

    // Drop the default layer(s) createBlank seeded; rebuild the stack from the file.
    std::vector<LayerId> seeded;
    for (const auto& layer : doc->topLevelLayers()) seeded.push_back(layer->id());
    for (LayerId id : seeded) (void)doc->cmdRemoveTopLevel(id);

    LayerId activeId = 0;
    bool haveActive = false;
    const auto depth = static_cast<BitDepth>(depthRaw);
    for (std::uint32_t i = 0; i < topCount; ++i) {
        auto layer = readLayer(r, canvasW, canvasH, depth, activeId, haveActive, 0);
        if (layer == nullptr) return nullptr;
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(layer));
    }

    if (haveActive) doc->setActiveLayer(activeId);
    return doc;
}

}  // namespace pe
