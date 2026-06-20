#include "pe/core/NativeFormat.hpp"

#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/BlendMode.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe/core/TextLayer.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef PHOTOEDIT_HAVE_ZLIB
#include <zlib.h>
#endif

namespace pe {

namespace {

// Magic is "PEDOC" + a version digit. v5 adds adjustment & solid-color layer records; v6 adds text
// layers. The reader checks only the "PEDOC" prefix + the u32 version, so it still accepts v4/v5
// files (they only ever contained the earlier kinds, which read identically).
constexpr char kMagic[6] = {'P', 'E', 'D', 'O', 'C', '6'};
constexpr std::uint32_t kVersion = 6;
constexpr std::uint32_t kMinReadVersion = 4;               // oldest format this reader accepts
constexpr std::int64_t kMaxLayerPixels = 64'000'000;       // per-layer content cap
constexpr std::int64_t kMaxTextRasterPixels = 16'000'000;  // matches the app's TextRender cap
constexpr std::uint32_t kMaxLayers = 100'000;
constexpr std::uint32_t kMaxNameBytes = 65'536;
constexpr int kMaxGroupDepth = 256;  // cap nesting so a hostile file can't blow the stack
constexpr std::uint32_t kMaxCurvePoints = 1024;  // bound a Curves adjustment's control-point count

// Layer kind discriminators written to the stream.
constexpr std::uint8_t kKindPixel = 0;
constexpr std::uint8_t kKindGroup = 1;
constexpr std::uint8_t kKindAdjustment = 2;
constexpr std::uint8_t kKindSolid = 3;
constexpr std::uint8_t kKindText = 4;

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

// Whether the format serializes this layer. Pixel, Group, Adjustment, SolidColor, and Text
// round-trip; embedded profiles arrive later.
[[nodiscard]] bool serializable(const Layer& layer) {
    return layer.kind() == LayerKind::Group || layer.isAdjustment() ||
           dynamic_cast<const SolidColorLayer*>(&layer) != nullptr ||
           dynamic_cast<const TextLayer*>(&layer) != nullptr ||
           dynamic_cast<const PixelLayer*>(&layer) != nullptr;
}

// --- adjustment-layer parameters (v5) ---------------------------------------------------------
// Serialized keyed by AdjustmentKind. The reader feeds every value back through the Adjustment
// setters/ctors, which clamp/sanitize their inputs, so out-of-range params from a hostile file
// produce a valid (clamped) adjustment rather than UB; non-finite floats are rejected outright.

void writeColorF(Writer& w, Rgbaf c) {
    w.f32(c.r);
    w.f32(c.g);
    w.f32(c.b);
    w.f32(c.a);
}

void writeAdjustment(Writer& w, const Adjustment& adj) {
    w.u8(static_cast<std::uint8_t>(adj.kind()));
    switch (adj.kind()) {
        case AdjustmentKind::BrightnessContrast: {
            const auto& a = static_cast<const BrightnessContrast&>(adj);
            w.f32(a.brightness());
            w.f32(a.contrast());
            break;
        }
        case AdjustmentKind::Levels: {
            const auto& a = static_cast<const Levels&>(adj);
            w.f32(a.inputBlack());
            w.f32(a.inputWhite());
            w.f32(a.gamma());
            w.f32(a.outputBlack());
            w.f32(a.outputWhite());
            break;
        }
        case AdjustmentKind::Curves: {
            const auto& a = static_cast<const Curves&>(adj);
            const auto& pts = a.points();
            w.u32(static_cast<std::uint32_t>(pts.size()));
            for (const auto& p : pts) {
                w.f32(p.first);
                w.f32(p.second);
            }
            break;
        }
        case AdjustmentKind::Invert:
            break;
        case AdjustmentKind::Exposure: {
            const auto& a = static_cast<const Exposure&>(adj);
            w.f32(a.stops());
            w.f32(a.offset());
            w.f32(a.gamma());
            break;
        }
        case AdjustmentKind::HueSaturation: {
            const auto& a = static_cast<const HueSaturation&>(adj);
            w.f32(a.hueShiftDegrees());
            w.f32(a.saturationScale());
            w.f32(a.lightness());
            w.u8(a.colorize() ? 1 : 0);
            w.f32(a.colorizeHue());
            w.f32(a.colorizeSat());
            break;
        }
        case AdjustmentKind::ChannelMixer: {
            const auto& a = static_cast<const ChannelMixer&>(adj);
            for (int o = 0; o < 3; ++o) {
                for (int i = 0; i < 4; ++i) w.f32(a.coeff(o, i));
            }
            w.u8(a.monochrome() ? 1 : 0);
            break;
        }
        case AdjustmentKind::GradientMap: {
            const auto& a = static_cast<const GradientMap&>(adj);
            writeColorF(w, a.color0());
            writeColorF(w, a.color1());
            w.u8(a.reverse() ? 1 : 0);
            break;
        }
        case AdjustmentKind::Vibrance: {
            const auto& a = static_cast<const Vibrance&>(adj);
            w.f32(a.vibrance());
            w.f32(a.saturation());
            break;
        }
        case AdjustmentKind::ColorBalance: {
            const auto& a = static_cast<const ColorBalance&>(adj);
            for (int c = 0; c < 3; ++c) w.f32(a.shadow(c));
            for (int c = 0; c < 3; ++c) w.f32(a.midtone(c));
            for (int c = 0; c < 3; ++c) w.f32(a.highlight(c));
            w.u8(a.preserveLuminosity() ? 1 : 0);
            break;
        }
        case AdjustmentKind::BlackAndWhite: {
            const auto& a = static_cast<const BlackAndWhite&>(adj);
            for (int b = 0; b < BlackAndWhite::kBandCount; ++b) w.f32(a.band(b));
            break;
        }
        case AdjustmentKind::PhotoFilter: {
            const auto& a = static_cast<const PhotoFilter&>(adj);
            writeColorF(w, a.color());
            w.f32(a.density());
            w.u8(a.preserveLuminosity() ? 1 : 0);
            break;
        }
        case AdjustmentKind::Posterize: {
            const auto& a = static_cast<const Posterize&>(adj);
            w.i32(a.levels());
            break;
        }
        case AdjustmentKind::Threshold: {
            const auto& a = static_cast<const Threshold&>(adj);
            w.f32(a.level());
            break;
        }
        case AdjustmentKind::SelectiveColor: {
            const auto& a = static_cast<const SelectiveColor&>(adj);
            for (int rr = 0; rr < SelectiveColor::kRangeCount; ++rr) {
                const auto cm = a.range(rr);
                w.f32(cm.c);
                w.f32(cm.m);
                w.f32(cm.y);
                w.f32(cm.k);
            }
            w.u8(a.relative() ? 1 : 0);
            break;
        }
    }
}

// Read an adjustment record. Returns nullptr on truncation, an unknown kind, an over-cap Curves
// point count, or any non-finite float (a valid file never writes those).
std::unique_ptr<Adjustment> readAdjustment(Reader& r) {
    const std::uint8_t kraw = r.u8();
    if (!r.ok() || kraw > static_cast<std::uint8_t>(AdjustmentKind::SelectiveColor)) return nullptr;
    const auto kind = static_cast<AdjustmentKind>(kraw);

    bool finite = true;
    const auto rf = [&]() -> float {
        const float v = r.f32();
        if (!std::isfinite(v)) finite = false;
        return v;
    };
    const auto readColor = [&]() -> Rgbaf {
        const float cr = rf();
        const float cg = rf();
        const float cb = rf();
        const float ca = rf();
        return Rgbaf{cr, cg, cb, ca};
    };
    const auto bad = [&]() { return !r.ok() || !finite; };

    switch (kind) {
        case AdjustmentKind::BrightnessContrast: {
            const float b = rf();
            const float c = rf();
            if (bad()) return nullptr;
            return std::make_unique<BrightnessContrast>(b, c);
        }
        case AdjustmentKind::Levels: {
            const float ib = rf();
            const float iw = rf();
            const float g = rf();
            const float ob = rf();
            const float ow = rf();
            if (bad()) return nullptr;
            auto a = std::make_unique<Levels>();
            a->setInputBlack(ib);
            a->setInputWhite(iw);
            a->setGamma(g);
            a->setOutputBlack(ob);
            a->setOutputWhite(ow);
            return a;
        }
        case AdjustmentKind::Curves: {
            const std::uint32_t n = r.u32();
            if (!r.ok() || n > kMaxCurvePoints) return nullptr;
            std::vector<std::pair<float, float>> pts;
            pts.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const float x = rf();
                const float y = rf();
                pts.emplace_back(x, y);
            }
            if (bad()) return nullptr;
            auto a = std::make_unique<Curves>();
            a->setPoints(std::move(pts));  // sanitizes (sorts / x-monotonic)
            return a;
        }
        case AdjustmentKind::Invert:
            return std::make_unique<Invert>();
        case AdjustmentKind::Exposure: {
            const float s = rf();
            const float o = rf();
            const float g = rf();
            if (bad()) return nullptr;
            return std::make_unique<Exposure>(s, o, g);
        }
        case AdjustmentKind::HueSaturation: {
            const float hue = rf();
            const float sat = rf();
            const float light = rf();
            const std::uint8_t col = r.u8();
            const float chue = rf();
            const float csat = rf();
            if (bad() || col > 1) return nullptr;
            auto a = std::make_unique<HueSaturation>();
            a->setHueShiftDegrees(hue);
            a->setSaturationScale(sat);
            a->setLightness(light);
            a->setColorize(col != 0, chue, csat);
            return a;
        }
        case AdjustmentKind::ChannelMixer: {
            float m[3][4];
            for (auto& row : m) {
                for (float& v : row) v = rf();
            }
            const std::uint8_t mono = r.u8();
            if (bad() || mono > 1) return nullptr;
            auto a = std::make_unique<ChannelMixer>();
            for (int o = 0; o < 3; ++o) a->setRow(o, m[o][0], m[o][1], m[o][2], m[o][3]);
            a->setMonochrome(mono != 0);
            return a;
        }
        case AdjustmentKind::GradientMap: {
            const Rgbaf c0 = readColor();
            const Rgbaf c1 = readColor();
            const std::uint8_t rev = r.u8();
            if (bad() || rev > 1) return nullptr;
            auto a = std::make_unique<GradientMap>(c0, c1);
            a->setReverse(rev != 0);
            return a;
        }
        case AdjustmentKind::Vibrance: {
            const float v = rf();
            const float s = rf();
            if (bad()) return nullptr;
            return std::make_unique<Vibrance>(v, s);
        }
        case AdjustmentKind::ColorBalance: {
            float sh[3];
            float mid[3];
            float hi[3];
            for (float& v : sh) v = rf();
            for (float& v : mid) v = rf();
            for (float& v : hi) v = rf();
            const std::uint8_t pl = r.u8();
            if (bad() || pl > 1) return nullptr;
            auto a = std::make_unique<ColorBalance>();
            a->setShadows(sh[0], sh[1], sh[2]);
            a->setMidtones(mid[0], mid[1], mid[2]);
            a->setHighlights(hi[0], hi[1], hi[2]);
            a->setPreserveLuminosity(pl != 0);
            return a;
        }
        case AdjustmentKind::BlackAndWhite: {
            float bands[BlackAndWhite::kBandCount];
            for (float& v : bands) v = rf();
            if (bad()) return nullptr;
            auto a = std::make_unique<BlackAndWhite>();
            for (int b = 0; b < BlackAndWhite::kBandCount; ++b) a->setBand(b, bands[b]);
            return a;
        }
        case AdjustmentKind::PhotoFilter: {
            const Rgbaf c = readColor();
            const float density = rf();
            const std::uint8_t pl = r.u8();
            if (bad() || pl > 1) return nullptr;
            auto a = std::make_unique<PhotoFilter>(c, density);
            a->setPreserveLuminosity(pl != 0);
            return a;
        }
        case AdjustmentKind::Posterize: {
            const std::int32_t levels = r.i32();
            if (!r.ok()) return nullptr;
            return std::make_unique<Posterize>(levels);  // setLevels clamps to [2,255]
        }
        case AdjustmentKind::Threshold: {
            const float level = rf();
            if (bad()) return nullptr;
            return std::make_unique<Threshold>(level);
        }
        case AdjustmentKind::SelectiveColor: {
            SelectiveColor::Cmyk ranges[SelectiveColor::kRangeCount];
            for (auto& cm : ranges) {
                cm.c = rf();
                cm.m = rf();
                cm.y = rf();
                cm.k = rf();
            }
            const std::uint8_t rel = r.u8();
            if (bad() || rel > 1) return nullptr;
            auto a = std::make_unique<SelectiveColor>();
            for (int rr = 0; rr < SelectiveColor::kRangeCount; ++rr) {
                a->setRange(rr, ranges[rr].c, ranges[rr].m, ranges[rr].y, ranges[rr].k);
            }
            a->setRelative(rel != 0);
            return a;
        }
    }
    return nullptr;
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

// Resident memory a sparse 256x256-tiled store commits when `rect`'s pixels are scattered
// into it: setPixel/setValue materializes every tile the rect touches IN FULL, so a thin
// or elongated rect costs far more than its dense pixel count (up to ~256x). The aggregate
// budget must charge this tiled footprint — not the packed rect bytes — or a crafted
// thin-strip content rect bypasses the cap. `bytesPerPixel` is the store's element size.
[[nodiscard]] std::int64_t tileFootprintBytes(Rect rect, std::int64_t bytesPerPixel) noexcept {
    const std::int64_t tiles = tilesForRect(rect).count();  // 0 for an empty rect
    return tiles * static_cast<std::int64_t>(kTilePixels) * bytesPerPixel;
}

// Read a byte block of exactly `expectedSize` bytes (written by writeBlock) into `out`.
// Returns false on any inconsistency (bad flag, missing zlib, truncation, size mismatch).
//
// `budget` is the remaining aggregate byte allowance for the whole deserialize and
// `chargeBytes` is what this block costs against it. The allocation guard is the security
// chokepoint: every block this format reads — pixel data and masks alike — flows through
// here, so checking the charge against the remaining budget BEFORE allocating bounds total
// memory and rejects a many-layer file or decompression bomb cleanly (false) instead of
// running into bad_alloc. Callers pass the RESIDENT tiled footprint (>= expectedSize) as
// `chargeBytes`, because the bytes are scattered into a sparse tiled store: charging the
// dense `expectedSize` would under-count by up to ~256x. The transient decompressed buffer
// is <= expectedSize <= chargeBytes, so this single charge bounds both.
bool readBlock(Reader& r, std::size_t expectedSize, std::vector<std::byte>& out,
               std::int64_t& budget, std::int64_t chargeBytes) {
    if (chargeBytes > budget) return false;  // aggregate cap (checked before allocating)
    budget -= chargeBytes;

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

// Serialize one layer's pixel store: content rect (clamped to the canvas) followed by a
// writeBlock of its pixels, sizeof(Pixel) bytes each. Templated over the store's pixel
// type so the 8/16/32-bit stores share one path (Rgba8/Rgba16/Rgbaf differ only in size).
template <class Pixel>
void writePixelBlock(Writer& w, const TileStoreT<Pixel>& store, Rect canvasBounds) {
    const Rect b = store.contentBounds().intersected(canvasBounds);
    w.i32(b.x);
    w.i32(b.y);
    w.i32(b.width);
    w.i32(b.height);
    std::vector<std::byte> raw;
    raw.reserve(static_cast<std::size_t>(b.width) * static_cast<std::size_t>(b.height) *
                sizeof(Pixel));
    for (int y = b.y; y < b.y + b.height; ++y) {
        for (int x = b.x; x < b.x + b.width; ++x) {
            const Pixel px = store.pixel(x, y);
            const auto* pb = reinterpret_cast<const std::byte*>(&px);
            raw.insert(raw.end(), pb, pb + sizeof(px));
        }
    }
    writeBlock(w, raw);
}

// Inverse of writePixelBlock for an already-validated content rect [cx,cy,cw,ch] (within
// the canvas and the per-layer pixel cap). Reads the pixel block (budget-checked) and
// scatters it into `store`. Templated over the pixel type as the write side is.
template <class Pixel>
bool readPixelBlock(Reader& r, TileStoreT<Pixel>& store, std::int32_t cx, std::int32_t cy,
                    std::int32_t cw, std::int32_t ch, std::int64_t& budget) {
    // cw*ch is capped at kMaxLayerPixels (readContentRect), so rawSize <= 64MP*sizeof(Pixel)
    // (<=1 GB even for 32-bit-float). Charge the budget the tiled footprint the scatter
    // below actually commits (whole tiles), not the dense rawSize, so a thin strip cannot
    // bypass the aggregate cap.
    const std::size_t rawSize =
        static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch) * sizeof(Pixel);
    const std::int64_t charge =
        tileFootprintBytes(Rect{cx, cy, cw, ch}, static_cast<std::int64_t>(sizeof(Pixel)));
    std::vector<std::byte> raw;
    if (!readBlock(r, rawSize, raw, budget, charge)) return false;
    std::size_t off = 0;
    for (int y = cy; y < cy + ch; ++y) {
        for (int x = cx; x < cx + cw; ++x) {
            Pixel px{};
            std::memcpy(&px, raw.data() + off, sizeof(px));
            off += sizeof(px);
            store.setPixel(x, y, px);
        }
    }
    return true;
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

void writeLayer(Writer& w, const Layer& layer, Rect canvasBounds, LayerId active, BitDepth depth) {
    const bool isGroup = layer.kind() == LayerKind::Group;
    const bool isAdjustment = layer.isAdjustment();
    const auto* solid = dynamic_cast<const SolidColorLayer*>(&layer);
    const auto* text = dynamic_cast<const TextLayer*>(&layer);
    const std::uint8_t kindByte = isGroup            ? kKindGroup
                                  : isAdjustment     ? kKindAdjustment
                                  : solid != nullptr ? kKindSolid
                                  : text != nullptr  ? kKindText
                                                     : kKindPixel;
    w.u8(kindByte);
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
        for (const Layer* child : kids) writeLayer(w, *child, canvasBounds, active, depth);
        return;
    }

    if (isAdjustment) {
        writeAdjustment(w, static_cast<const AdjustmentLayer&>(layer).adjustment());
        return;
    }

    if (solid != nullptr) {
        const Rgba8 c = solid->color();
        w.u8(c.r);
        w.u8(c.g);
        w.u8(c.b);
        w.u8(c.a);
        // Clamp the fill rect to the canvas so it round-trips through readContentRect (which
        // rejects off-canvas rects); the part outside the canvas isn't visible anyway.
        const Rect b = solid->bounds().intersected(canvasBounds);
        w.i32(b.x);
        w.i32(b.y);
        w.i32(b.width);
        w.i32(b.height);
        return;
    }

    if (text != nullptr) {
        // Editable model (re-rasterized by the app on edit).
        const TextModel& m = text->model();
        w.u32(static_cast<std::uint32_t>(m.text.size()));
        w.bytes(m.text.data(), m.text.size());
        w.u32(static_cast<std::uint32_t>(m.fontFamily.size()));
        w.bytes(m.fontFamily.data(), m.fontFamily.size());
        w.i32(m.pixelSize);
        w.u8(m.bold ? 1 : 0);
        w.u8(m.italic ? 1 : 0);
        w.u8(m.color.r);
        w.u8(m.color.g);
        w.u8(m.color.b);
        w.u8(m.color.a);
        w.i32(m.origin.x);
        w.i32(m.origin.y);
        // Cached straight-alpha RGBA8 raster (the engine can't regenerate it). Not canvas-clamped:
        // text may legitimately hang off the edge, so it carries its own placement + dims.
        const PixelBuffer& ras = text->raster();
        const Point ro = text->rasterOrigin();
        w.i32(ro.x);
        w.i32(ro.y);
        w.i32(ras.width());
        w.i32(ras.height());
        const std::size_t n = static_cast<std::size_t>(ras.width()) *
                              static_cast<std::size_t>(ras.height()) * sizeof(Rgba8);
        std::vector<std::byte> raw;
        if (n > 0) {
            const auto* pb = reinterpret_cast<const std::byte*>(ras.data());
            raw.assign(pb, pb + n);
        }
        writeBlock(w, raw);
        return;
    }

    // Pixel layer: serialize the store matching the document's bit depth (the active
    // store; the others are empty). `depth` is the document depth, which every pixel
    // layer shares (createBlank/AddLayer seed layers at the document depth) — the reader
    // reconstructs all layers at that same depth, so writer and reader stay symmetric.
    //
    // PRECONDITION: every PixelLayer in the document has pl.depth() == doc.bitDepth(). The
    // format keys pixel serialization off the document depth (it stores no per-layer depth),
    // so a layer whose own depth diverged would serialize its empty matching store and
    // round-trip transparent. No current path can produce a mismatch; a future mixed-depth
    // path (paste/import) must add a per-layer depth byte here and in readLayer.
    const auto& pl = static_cast<const PixelLayer&>(layer);
    switch (depth) {
        case BitDepth::U16:
            writePixelBlock(w, pl.tiles16(), canvasBounds);
            break;
        case BitDepth::F32:
            writePixelBlock(w, pl.tilesF(), canvasBounds);
            break;
        case BitDepth::U8:
        default:
            writePixelBlock(w, pl.tiles(), canvasBounds);
            break;
    }
}

// Reads one layer (recursively for groups). Returns nullptr on any inconsistency.
// Sets activeId/haveActive if a record carries the active flag.
std::unique_ptr<Layer> readLayer(Reader& r, int canvasW, int canvasH, BitDepth depth,
                                 LayerId& activeId, bool& haveActive, int depthGuard,
                                 std::int64_t& budget, std::int64_t& nodeCount) {
    if (depthGuard > kMaxGroupDepth) return nullptr;
    // Global node cap across the WHOLE tree: the childCount/topCount checks are per-level only, so
    // without this a crafted file could declare ~kMaxLayers groups each with ~kMaxLayers (zero-
    // content) children — millions of layer objects, none charging the byte budget — and OOM.
    if (++nodeCount > kMaxLayers) return nullptr;

    const std::uint8_t kind = r.u8();
    const bool visible = r.u8() != 0;
    const float opacity = r.f32();
    const std::uint8_t blendRaw = r.u8();
    const std::uint8_t activeFlag = r.u8();
    const std::uint32_t nameLen = r.u32();
    if (!r.ok() || kind > kKindText || blendRaw >= static_cast<std::uint8_t>(BlendMode::Count) ||
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
        // MaskBuffer is a sparse store of 1-byte tiles, so charge its tiled footprint too.
        const std::int64_t mcharge = tileFootprintBytes(Rect{mx, my, mw, mh}, 1);
        if (!readBlock(r, static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh), mraw, budget,
                       mcharge)) {
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
            auto child = readLayer(r, canvasW, canvasH, depth, activeId, haveActive, depthGuard + 1,
                                   budget, nodeCount);
            if (child == nullptr) return nullptr;
            group->addChild(std::move(child));
        }
        layer = std::move(group);
    } else if (kind == kKindAdjustment) {
        auto adj = readAdjustment(r);
        if (adj == nullptr) return nullptr;
        layer = std::make_unique<AdjustmentLayer>(std::move(adj), std::move(name));
    } else if (kind == kKindSolid) {
        const std::uint8_t cr = r.u8();
        const std::uint8_t cg = r.u8();
        const std::uint8_t cb = r.u8();
        const std::uint8_t ca = r.u8();
        const std::int32_t bx = r.i32();
        const std::int32_t by = r.i32();
        const std::int32_t bw = r.i32();
        const std::int32_t bh = r.i32();
        // Validate against the canvas (the writer clamps to it). No pixel cap: a solid fill is
        // procedural (stores only the rect), so a full-canvas rect is fine even past
        // kMaxLayerPixels.
        if (!r.ok() || bw < 0 || bh < 0 || bx < 0 || by < 0 ||
            static_cast<std::int64_t>(bx) + bw > canvasW ||
            static_cast<std::int64_t>(by) + bh > canvasH) {
            return nullptr;
        }
        layer = std::make_unique<SolidColorLayer>(Rgba8{cr, cg, cb, ca}, Rect{bx, by, bw, bh},
                                                  std::move(name));
    } else if (kind == kKindText) {
        // Editable model.
        const std::uint32_t tlen = r.u32();
        if (!r.ok() || tlen > kMaxNameBytes) return nullptr;
        std::string ttext(tlen, '\0');
        if (tlen > 0 && !r.read(ttext.data(), tlen)) return nullptr;
        const std::uint32_t flen = r.u32();
        if (!r.ok() || flen > kMaxNameBytes) return nullptr;
        std::string family(flen, '\0');
        if (flen > 0 && !r.read(family.data(), flen)) return nullptr;
        const std::int32_t pixelSize = r.i32();
        const std::uint8_t bold = r.u8();
        const std::uint8_t italic = r.u8();
        const std::uint8_t cr = r.u8();
        const std::uint8_t cg = r.u8();
        const std::uint8_t cb = r.u8();
        const std::uint8_t ca = r.u8();
        const std::int32_t ox = r.i32();
        const std::int32_t oy = r.i32();
        // Cached raster: its own placement + dims, validated INDEPENDENTLY of the canvas (text may
        // hang off the edge). A flat PixelBuffer is resident-dense, so charge exactly its bytes.
        const std::int32_t rox = r.i32();
        const std::int32_t roy = r.i32();
        const std::int32_t rw = r.i32();
        const std::int32_t rh = r.i32();
        if (!r.ok() || bold > 1 || italic > 1 || rw < 0 || rh < 0 || rw > kMaxCanvasDimension ||
            rh > kMaxCanvasDimension || static_cast<std::int64_t>(rw) * rh > kMaxTextRasterPixels ||
            rox < -kMaxCanvasDimension || rox > kMaxCanvasDimension || roy < -kMaxCanvasDimension ||
            roy > kMaxCanvasDimension) {
            return nullptr;
        }
        const std::size_t rawSize =
            static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh) * sizeof(Rgba8);
        std::vector<std::byte> raw;
        if (!readBlock(r, rawSize, raw, budget, static_cast<std::int64_t>(rawSize))) return nullptr;
        PixelBuffer raster(rw, rh);
        if (rawSize > 0) std::memcpy(raster.data(), raw.data(), rawSize);
        // The model only drives the app's re-rasterize; sanitize pixelSize to a sane positive
        // range.
        const int ps = pixelSize < 1                     ? 1
                       : pixelSize > kMaxCanvasDimension ? kMaxCanvasDimension
                                                         : pixelSize;
        TextModel model{std::move(ttext), std::move(family),     ps,           bold != 0,
                        italic != 0,      Rgba8{cr, cg, cb, ca}, Point{ox, oy}};
        layer = std::make_unique<TextLayer>(std::move(model), std::move(raster), Point{rox, roy},
                                            std::move(name));
    } else {
        auto pl = std::make_unique<PixelLayer>(std::move(name), depth);
        std::int32_t cx = 0;
        std::int32_t cy = 0;
        std::int32_t cw = 0;
        std::int32_t ch = 0;
        if (!readContentRect(r, canvasW, canvasH, cx, cy, cw, ch)) return nullptr;
        // Read into the store matching the document depth (the active one); the per-pixel
        // size and the aggregate budget are handled inside readPixelBlock/readBlock.
        bool okPixels = false;
        switch (depth) {
            case BitDepth::U16:
                okPixels = readPixelBlock(r, pl->tiles16(), cx, cy, cw, ch, budget);
                break;
            case BitDepth::F32:
                okPixels = readPixelBlock(r, pl->tilesF(), cx, cy, cw, ch, budget);
                break;
            case BitDepth::U8:
            default:
                okPixels = readPixelBlock(r, pl->tiles(), cx, cy, cw, ch, budget);
                break;
        }
        if (!okPixels) return nullptr;
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
    const BitDepth depth = doc.bitDepth();
    for (const Layer* layer : tops) writeLayer(w, *layer, canvasBounds, doc.activeLayer(), depth);
    return w.take();
}

std::unique_ptr<Document> deserializeDocument(std::span<const std::byte> data,
                                              std::int64_t maxTotalContentBytes) {
    Reader r(data);

    char magic[6] = {};
    // Check the 5-char "PEDOC" prefix; the 6th byte is the version digit, redundant with the u32
    // version below (which is authoritative and gates the accepted range).
    if (!r.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, 5) != 0) {
        return nullptr;
    }
    const std::uint32_t ver = r.u32();
    if (!r.ok() || ver < kMinReadVersion || ver > kVersion) return nullptr;

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
    // Remaining aggregate allocation allowance for this whole deserialize (clamped to
    // non-negative). Every pixel/mask block read decrements it before allocating, so a
    // bomb or many-layer file is rejected rather than driven to bad_alloc.
    std::int64_t budget = maxTotalContentBytes < 0 ? 0 : maxTotalContentBytes;
    std::int64_t nodeCount = 0;  // total layers across the whole tree (global cap, not per-level)
    for (std::uint32_t i = 0; i < topCount; ++i) {
        auto layer =
            readLayer(r, canvasW, canvasH, depth, activeId, haveActive, 0, budget, nodeCount);
        if (layer == nullptr) return nullptr;
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(layer));
    }

    if (haveActive) doc->setActiveLayer(activeId);
    return doc;
}

}  // namespace pe
