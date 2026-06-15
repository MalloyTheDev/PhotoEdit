#include "pe/core/NativeFormat.hpp"

#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#ifdef PHOTOEDIT_HAVE_ZLIB
#include <zlib.h>
#endif

namespace pe {

namespace {

constexpr char kMagic[6] = {'P', 'E', 'D', 'O', 'C', '4'};
constexpr std::uint32_t kVersion = 4;
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
    // Borrow the next n bytes as a span without copying (advances pos_). Empty on
    // failure / out-of-range.
    std::span<const std::byte> readSpan(std::size_t n) {
        if (!ok_ || pos_ + n > data_.size()) {
            ok_ = false;
            return {};
        }
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
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

#ifdef PHOTOEDIT_HAVE_ZLIB
// Deflate `in`. Returns empty on failure (caller falls back to storing raw).
std::vector<std::byte> zlibDeflate(const std::vector<std::byte>& in) {
    if (in.empty()) return {};
    uLongf bound = compressBound(static_cast<uLong>(in.size()));
    std::vector<std::byte> out(bound);
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &bound,
                  reinterpret_cast<const Bytef*>(in.data()), static_cast<uLong>(in.size()),
                  Z_DEFAULT_COMPRESSION) != Z_OK) {
        return {};
    }
    out.resize(bound);
    return out;
}
// Inflate `in` into a buffer of exactly `expected` bytes. False on any error or if the
// decompressed size differs (guards against a lying length).
bool zlibInflate(std::span<const std::byte> in, std::vector<std::byte>& out) {
    uLongf outLen = static_cast<uLongf>(out.size());
    const int rc =
        uncompress(reinterpret_cast<Bytef*>(out.data()), &outLen,
                   reinterpret_cast<const Bytef*>(in.data()), static_cast<uLong>(in.size()));
    return rc == Z_OK && outLen == out.size();
}
#endif

// Whether the format serializes this layer (Pixel and Group in v3; adjustment/fill/
// text layers and embedded profiles round-trip later).
[[nodiscard]] bool serializable(const Layer& layer) {
    return layer.kind() == LayerKind::Group || dynamic_cast<const PixelLayer*>(&layer) != nullptr;
}

// Write a length-flagged byte block: zlib-compressed when that is smaller (and zlib is
// built in), else raw. The per-block flag keeps the surrounding record uncompressed.
void writeBlock(Writer& w, const std::vector<std::byte>& raw) {
#ifdef PHOTOEDIT_HAVE_ZLIB
    const std::vector<std::byte> comp = zlibDeflate(raw);
    if (!comp.empty() && comp.size() < raw.size()) {
        w.u8(1);  // compressed: u32 length + deflated bytes
        w.u32(static_cast<std::uint32_t>(comp.size()));
        w.bytes(comp.data(), comp.size());
        return;
    }
#endif
    w.u8(0);  // raw: expectedSize bytes follow
    w.bytes(raw.data(), raw.size());
}

// Read a byte block of exactly `expectedSize` bytes (written by writeBlock) into `out`.
// Returns false on any inconsistency (bad flag, missing zlib, truncation, size mismatch).
bool readBlock(Reader& r, std::size_t expectedSize, std::vector<std::byte>& out) {
    const std::uint8_t comp = r.u8();
    if (!r.ok() || comp > 1) return false;
    if (comp == 1) {
#ifdef PHOTOEDIT_HAVE_ZLIB
        const std::uint32_t clen = r.u32();
        const std::span<const std::byte> cspan = r.readSpan(clen);
        if (!r.ok()) return false;
        out.assign(expectedSize, std::byte{});
        return expectedSize == 0 || zlibInflate(cspan, out);
#else
        return false;  // compressed, but this build has no zlib to read it
#endif
    }
    out.assign(expectedSize, std::byte{});
    return expectedSize == 0 || r.read(out.data(), expectedSize);
}

// Read and validate a content rect that must lie within the canvas. Returns false on an
// out-of-canvas, overflowing, or oversized rect; on success cx/cy/cw/ch are filled. The
// int64 math keeps cx+cw / cy+ch from overflowing the int loop bounds and confines tile
// allocation to the canvas.
bool readContentRect(Reader& r, int canvasW, int canvasH, std::int32_t& cx, std::int32_t& cy,
                     std::int32_t& cw, std::int32_t& ch) {
    cx = r.i32();
    cy = r.i32();
    cw = r.i32();
    ch = r.i32();
    return r.ok() && cw >= 0 && ch >= 0 && cx >= 0 && cy >= 0 &&
           static_cast<std::int64_t>(cx) + cw <= canvasW &&
           static_cast<std::int64_t>(cy) + ch <= canvasH &&
           static_cast<std::int64_t>(cw) * ch <= kMaxLayerPixels;
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

    // Optional layer mask (common to pixel and group layers).
    const Mask* mask = layer.mask();
    if (mask == nullptr) {
        w.u8(0);
    } else {
        w.u8(1);
        w.u8(static_cast<std::uint8_t>(mask->kind()));
        w.u8(mask->enabled() ? 1 : 0);
        w.f32(mask->density());
        w.u8(mask->inverted() ? 1 : 0);
        const Rect mb = mask->buffer().contentBounds().intersected(canvasBounds);
        w.i32(mb.x);
        w.i32(mb.y);
        w.i32(mb.width);
        w.i32(mb.height);
        std::vector<std::byte> mraw;
        mraw.reserve(static_cast<std::size_t>(mb.width) * static_cast<std::size_t>(mb.height));
        for (int y = mb.y; y < mb.y + mb.height; ++y) {
            for (int x = mb.x; x < mb.x + mb.width; ++x) {
                mraw.push_back(static_cast<std::byte>(mask->buffer().value(x, y)));
            }
        }
        writeBlock(w, mraw);
    }

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

    std::vector<std::byte> raw;
    raw.reserve(static_cast<std::size_t>(b.width) * static_cast<std::size_t>(b.height) *
                sizeof(Rgba8));
    for (int y = b.y; y < b.y + b.height; ++y) {
        for (int x = b.x; x < b.x + b.width; ++x) {
            const Rgba8 px = pl.tiles().pixel(x, y);
            const auto* pb = reinterpret_cast<const std::byte*>(&px);
            raw.insert(raw.end(), pb, pb + sizeof(px));
        }
    }
    writeBlock(w, raw);
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

    // Optional layer mask (common to pixel and group layers).
    std::unique_ptr<Mask> mask;
    const std::uint8_t hasMask = r.u8();
    if (!r.ok() || hasMask > 1) return nullptr;
    if (hasMask == 1) {
        const std::uint8_t mkind = r.u8();
        const bool menabled = r.u8() != 0;
        const float mdensity = r.f32();
        const bool minverted = r.u8() != 0;
        std::int32_t mx = 0;
        std::int32_t my = 0;
        std::int32_t mw = 0;
        std::int32_t mh = 0;
        if (!r.ok() || mkind > static_cast<std::uint8_t>(Mask::Kind::Quick) ||
            !readContentRect(r, canvasW, canvasH, mx, my, mw, mh)) {
            return nullptr;
        }
        std::vector<std::byte> mraw;
        if (!readBlock(r, static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh), mraw)) {
            return nullptr;
        }
        mask = std::make_unique<Mask>(static_cast<Mask::Kind>(mkind));
        mask->setEnabled(menabled);
        mask->setDensity(mdensity);
        mask->setInverted(minverted);
        std::size_t off = 0;
        for (int y = my; y < my + mh; ++y) {
            for (int x = mx; x < mx + mw; ++x) {
                mask->buffer().setValue(x, y, static_cast<std::uint8_t>(mraw[off++]));
            }
        }
    }

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
        std::int32_t cx = 0;
        std::int32_t cy = 0;
        std::int32_t cw = 0;
        std::int32_t ch = 0;
        if (!readContentRect(r, canvasW, canvasH, cx, cy, cw, ch)) return nullptr;
        // cw*ch is capped at kMaxLayerPixels, so rawSize <= 256 MB.
        const std::size_t rawSize =
            static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch) * sizeof(Rgba8);
        std::vector<std::byte> raw;
        if (!readBlock(r, rawSize, raw)) return nullptr;

        TileStore& store = pl->tiles();
        std::size_t off = 0;
        for (int y = cy; y < cy + ch; ++y) {
            for (int x = cx; x < cx + cw; ++x) {
                Rgba8 px{};
                std::memcpy(&px, raw.data() + off, sizeof(px));
                off += sizeof(px);
                store.setPixel(x, y, px);
            }
        }
        layer = std::move(pl);
    }

    if (mask) layer->setMask(std::move(mask));
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
