#include "pe/core/ImageIO.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Hardened, dependency-free reader for the Photoshop PSD format. It reads ONLY the merged
// (composite) image written at the end of the file — not the layer stack — and flattens it to an
// 8-bit RGBA PixelBuffer, matching how the other raster codecs import. Every field that drives an
// allocation or a copy is bounds-checked against the input; the parser returns std::nullopt for
// anything malformed, truncated, oversized, or unsupported, and never reads out of bounds, writes
// out of bounds, or overflows on hostile input. See docs/systems/16-file-formats.md and the PSD
// spec (Adobe Photoshop File Formats Specification).

namespace pe {

namespace {

// Same 64 MP (RGBA8 == 256 MB) decode cap the other codecs use, plus the PSD spec's own per-
// dimension maximum (30000 for PSD/version 1). Both are enforced before any pixel allocation.
constexpr std::int64_t kMaxImagePixels = 64'000'000;
constexpr std::uint32_t kMaxPsdDimension = 30000;

// A forward, bounds-checked, big-endian cursor over the input. Any read past the end sets a
// sticky failure flag and yields zero, so callers can parse optimistically and check ok() at the
// end; no read ever touches memory outside the span.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return failed_ ? 0 : data_.size() - pos_;
    }

    std::uint8_t u8() noexcept {
        if (failed_ || pos_ >= data_.size()) {
            failed_ = true;
            return 0;
        }
        return static_cast<std::uint8_t>(data_[pos_++]);
    }
    std::uint16_t u16() noexcept {
        const std::uint32_t hi = u8();
        const std::uint32_t lo = u8();
        return static_cast<std::uint16_t>((hi << 8) | lo);
    }
    std::uint32_t u32() noexcept {
        const std::uint32_t a = u8();
        const std::uint32_t b = u8();
        const std::uint32_t c = u8();
        const std::uint32_t d = u8();
        return (a << 24) | (b << 16) | (c << 8) | d;
    }

    // Advance by n, or fail if that would pass the end. (n is widened so a 32-bit length from the
    // file cannot overflow the addition.)
    void skip(std::uint64_t n) noexcept {
        if (failed_ || n > data_.size() - pos_) {
            failed_ = true;
            return;
        }
        pos_ += static_cast<std::size_t>(n);
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Decode one PackBits (RLE) row of exactly `outWidth` bytes from the next `compressedLen` bytes of
// `r`. On success returns true and sets `consumed` to the number of compressed bytes actually read
// (always <= compressedLen); the caller realigns the cursor to the declared slice end. Returns
// false (without escaping the slice) if the row over-/under-runs or the input is exhausted — i.e.
// malformed/truncated data fails cleanly rather than corrupting memory.
bool unpackBitsRow(Reader& r, std::uint32_t compressedLen, int outWidth, std::uint8_t* out,
                   std::uint32_t& consumed) {
    consumed = 0;
    int written = 0;
    while (written < outWidth) {
        if (consumed >= compressedLen) return false;  // ran out of compressed data for this row
        const std::int8_t n = static_cast<std::int8_t>(r.u8());
        ++consumed;
        if (!r.ok()) return false;
        if (n >= 0) {
            const int count = static_cast<int>(n) + 1;  // 1..128 literal bytes
            if (written + count > outWidth ||
                consumed + static_cast<std::uint32_t>(count) > compressedLen) {
                return false;
            }
            for (int i = 0; i < count; ++i) {
                out[written++] = r.u8();
                ++consumed;
            }
        } else if (n != -128) {
            const int count = 1 - static_cast<int>(n);  // 2..128 repeats
            if (written + count > outWidth || consumed >= compressedLen) return false;
            const std::uint8_t v = r.u8();
            ++consumed;
            for (int i = 0; i < count; ++i) out[written++] = v;
        }
        // n == -128 is a no-op per the PackBits spec.
        if (!r.ok()) return false;
    }
    return written == outWidth;
}

}  // namespace

std::optional<PixelBuffer> decodePsd(std::span<const std::byte> data) {
    Reader r(data);

    // --- File header (26 bytes) ---
    if (r.u8() != '8' || r.u8() != 'B' || r.u8() != 'P' || r.u8() != 'S') return std::nullopt;
    const std::uint16_t version = r.u16();
    if (version != 1) return std::nullopt;  // 2 == PSB (8-byte lengths); not supported in v1
    r.skip(6);                              // 6 reserved bytes (spec: must be zero)
    const std::uint16_t channels = r.u16();
    const std::uint32_t height = r.u32();
    const std::uint32_t width = r.u32();
    const std::uint16_t depth = r.u16();
    const std::uint16_t colorMode = r.u16();
    if (!r.ok()) return std::nullopt;

    // Validate before allocating anything.
    if (width == 0 || height == 0 || width > kMaxPsdDimension || height > kMaxPsdDimension) {
        return std::nullopt;
    }
    if (static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height) > kMaxImagePixels) {
        return std::nullopt;
    }
    if (depth != 8) return std::nullopt;  // 1/16/32-bit composites not supported in v1
    // Supported color modes and how many leading channels they consume.
    int colorChannels = 0;
    if (colorMode == 1) {
        colorChannels = 1;  // Grayscale
    } else if (colorMode == 3) {
        colorChannels = 3;  // RGB
    } else {
        return std::nullopt;  // Bitmap/Indexed/CMYK/Multichannel/Duotone/Lab: not supported
    }
    if (channels < colorChannels || channels > 56) return std::nullopt;  // spec caps at 56
    const bool hasAlpha = channels > colorChannels;  // first extra channel is the composite alpha

    // --- Three variable-length sections we don't need: skip each by its declared length. ---
    r.skip(r.u32());  // Color Mode Data
    if (!r.ok()) return std::nullopt;
    r.skip(r.u32());  // Image Resources
    if (!r.ok()) return std::nullopt;
    r.skip(r.u32());  // Layer and Mask Information (we read the composite, not the layers)
    if (!r.ok()) return std::nullopt;

    // --- Image Data section: the composite, all channels planar ---
    const std::uint16_t compression = r.u16();
    if (!r.ok()) return std::nullopt;
    if (compression != 0 && compression != 1) return std::nullopt;  // 2/3 == ZIP: not supported

    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const std::size_t plane = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    // Decode only the channels we actually use (color planes + optional alpha); any further
    // channels (spot/mask) are walked past but not stored, bounding memory to <= 4 planes.
    const int storedChannels = colorChannels + (hasAlpha ? 1 : 0);

    // For RLE, the section begins with a per-scanline compressed-byte-count table covering EVERY
    // channel (channels * height entries, u16 each). Read it up front so each row can be decoded
    // from an exact slice.
    std::vector<std::uint32_t> rowLengths;
    if (compression == 1) {
        const std::uint64_t rows = static_cast<std::uint64_t>(channels) * height;
        if (rows > r.remaining() / 2) return std::nullopt;  // table itself would overrun input
        rowLengths.resize(static_cast<std::size_t>(rows));
        for (std::uint64_t i = 0; i < rows; ++i) rowLengths[static_cast<std::size_t>(i)] = r.u16();
        if (!r.ok()) return std::nullopt;
    }

    std::vector<std::vector<std::uint8_t>> planes(static_cast<std::size_t>(storedChannels));
    std::vector<std::uint8_t> scratch;  // reused for discarded channels' rows

    for (int ch = 0; ch < static_cast<int>(channels); ++ch) {
        const bool store = ch < storedChannels;
        std::vector<std::uint8_t>* dst = nullptr;
        if (store) {
            planes[static_cast<std::size_t>(ch)].resize(plane);
            dst = &planes[static_cast<std::size_t>(ch)];
        }
        if (compression == 0) {  // raw: w*h contiguous bytes for this channel
            if (plane > r.remaining()) return std::nullopt;
            for (std::size_t i = 0; i < plane; ++i) {
                const std::uint8_t v = r.u8();
                if (store) (*dst)[i] = v;
            }
            if (!r.ok()) return std::nullopt;
        } else {  // RLE: h scanlines, each occupying exactly its tabled compressed length
            for (int y = 0; y < h; ++y) {
                const std::size_t idx = static_cast<std::size_t>(ch) * static_cast<std::size_t>(h) +
                                        static_cast<std::size_t>(y);
                const std::uint32_t clen = rowLengths[idx];
                if (clen > r.remaining()) return std::nullopt;
                const std::size_t rowStart =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
                std::uint8_t* target = nullptr;
                if (store) {
                    target = dst->data() + rowStart;
                } else {
                    scratch.assign(static_cast<std::size_t>(w), 0);
                    target = scratch.data();
                }
                std::uint32_t consumed = 0;
                if (!unpackBitsRow(r, clen, w, target, consumed)) return std::nullopt;
                // A scanline occupies exactly clen bytes; skip any unused tail (consumed <= clen)
                // so a row that decoded in fewer bytes than declared cannot desync the next one.
                r.skip(clen - consumed);
                if (!r.ok()) return std::nullopt;
            }
        }
    }

    // --- Assemble RGBA8 from the decoded planes ---
    PixelBuffer out(w, h);
    const std::uint8_t* a =
        hasAlpha ? planes[static_cast<std::size_t>(colorChannels)].data() : nullptr;
    if (colorChannels == 1) {
        const std::uint8_t* g = planes[0].data();
        for (std::size_t i = 0; i < plane; ++i) {
            const std::uint8_t v = g[i];
            out.data()[i] = Rgba8{v, v, v, a ? a[i] : std::uint8_t{255}};
        }
    } else {  // RGB
        const std::uint8_t* rp = planes[0].data();
        const std::uint8_t* gp = planes[1].data();
        const std::uint8_t* bp = planes[2].data();
        for (std::size_t i = 0; i < plane; ++i) {
            out.data()[i] = Rgba8{rp[i], gp[i], bp[i], a ? a[i] : std::uint8_t{255}};
        }
    }
    return out;
}

}  // namespace pe
