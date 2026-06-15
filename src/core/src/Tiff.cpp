#include "pe/core/ImageIO.hpp"

#include <tiffio.h>

#include <cstdint>
#include <cstring>

namespace pe {

namespace {
// Cap decoded dimensions before allocating (untrusted input); 64 MP of RGBA8 = 256 MB.
constexpr std::int64_t kMaxImagePixels = 64'000'000;

// --- in-memory libtiff client adapters (libtiff has no built-in memory API) ---

struct MemReader {
    const std::byte* data;
    toff_t size;
    toff_t pos;
};

struct MemWriter {
    std::vector<std::byte> data;
    toff_t pos = 0;
};

tsize_t readRead(thandle_t h, tdata_t buf, tsize_t n) {
    auto* m = static_cast<MemReader*>(h);
    const toff_t want = n > 0 ? static_cast<toff_t>(n) : 0;
    const toff_t avail = m->pos < m->size ? m->size - m->pos : 0;
    const toff_t k = want < avail ? want : avail;
    if (k > 0) std::memcpy(buf, m->data + m->pos, static_cast<std::size_t>(k));
    m->pos += k;
    return static_cast<tsize_t>(k);
}
tsize_t readWrite(thandle_t, tdata_t, tsize_t) {
    return 0;
}  // read-mode: no writes
toff_t readSeek(thandle_t h, toff_t off, int whence) {
    auto* m = static_cast<MemReader*>(h);
    toff_t np = whence == SEEK_CUR ? m->pos + off : (whence == SEEK_END ? m->size + off : off);
    m->pos = np;
    return np;
}
toff_t readSize(thandle_t h) {
    return static_cast<MemReader*>(h)->size;
}

tsize_t writeRead(thandle_t, tdata_t, tsize_t) {
    return 0;
}
tsize_t writeWrite(thandle_t h, tdata_t buf, tsize_t n) {
    auto* m = static_cast<MemWriter*>(h);
    const std::size_t end = static_cast<std::size_t>(m->pos) + static_cast<std::size_t>(n);
    if (end > m->data.size()) m->data.resize(end);
    std::memcpy(m->data.data() + m->pos, buf, static_cast<std::size_t>(n));
    m->pos += n;
    return n;
}
toff_t writeSeek(thandle_t h, toff_t off, int whence) {
    auto* m = static_cast<MemWriter*>(h);
    const toff_t cur = static_cast<toff_t>(m->data.size());
    toff_t np = whence == SEEK_CUR ? m->pos + off : (whence == SEEK_END ? cur + off : off);
    m->pos = np;
    return np;
}
toff_t writeSize(thandle_t h) {
    return static_cast<toff_t>(static_cast<MemWriter*>(h)->data.size());
}

// Validate the TIFF header ourselves before handing bytes to libtiff: "II"+42/43
// (little-endian) or "MM"+42/43 (big-endian; 43 == BigTIFF). This rejects garbage up
// front so libtiff's parser never sees malformed input — some libtiff builds crash on
// certain malformed streams via a memory client.
bool looksLikeTiff(std::span<const std::byte> d) {
    if (d.size() < 8) return false;
    const auto b0 = std::to_integer<unsigned char>(d[0]);
    const auto b1 = std::to_integer<unsigned char>(d[1]);
    const auto b2 = std::to_integer<unsigned char>(d[2]);
    const auto b3 = std::to_integer<unsigned char>(d[3]);
    if (b0 == 'I' && b1 == 'I' && (b2 == 0x2A || b2 == 0x2B) && b3 == 0x00) return true;
    if (b0 == 'M' && b1 == 'M' && b2 == 0x00 && (b3 == 0x2A || b3 == 0x2B)) return true;
    return false;
}

int memClose(thandle_t) {
    return 0;
}
int memMap(thandle_t, tdata_t*, toff_t*) {
    return 0;
}  // no memory mapping
void memUnmap(thandle_t, tdata_t, toff_t) {}
}  // namespace

std::vector<std::byte> encodeTiff(const PixelBuffer& image) {
    if (image.isEmpty()) return {};

    MemWriter sink;
    TIFF* tif = TIFFClientOpen("mem", "w", static_cast<thandle_t>(&sink), writeRead, writeWrite,
                               writeSeek, memClose, writeSize, memMap, memUnmap);
    if (tif == nullptr) return {};

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(image.width()));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(image.height()));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);  // lossless
    const uint16_t extra[1] = {EXTRASAMPLE_UNASSALPHA};       // straight (unassociated) alpha
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, extra);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));

    bool ok = true;
    for (int y = 0; y < image.height() && ok; ++y) {
        // One contiguous RGBA8 scanline; TIFFWriteScanline takes a non-const buffer but
        // does not modify it for a write.
        auto* row = const_cast<Rgba8*>(image.data() + static_cast<std::size_t>(y) * image.width());
        ok = TIFFWriteScanline(tif, row, static_cast<uint32_t>(y), 0) >= 0;
    }
    TIFFClose(tif);
    if (!ok) return {};
    return std::move(sink.data);
}

std::optional<PixelBuffer> decodeTiff(std::span<const std::byte> data) {
    // Reject non-TIFF input before libtiff ever parses it: cheap, and avoids feeding
    // garbage to libtiff's parser (some builds crash on certain malformed streams).
    if (!looksLikeTiff(data)) return std::nullopt;

    // Silence libtiff's process-global warning/error handlers (they print to stderr on
    // malformed input and behave inconsistently across builds). Idempotent.
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);

    MemReader src{data.data(), static_cast<toff_t>(data.size()), 0};
    TIFF* tif = TIFFClientOpen("mem", "r", static_cast<thandle_t>(&src), readRead, readWrite,
                               readSeek, memClose, readSize, memMap, memUnmap);
    if (tif == nullptr) return std::nullopt;

    uint32_t w = 0;
    uint32_t h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    // uint64 product so two 32-bit dimensions can't overflow before the cap compares
    // (0xFFFFFFFF^2 fits in uint64) — matches the PNG/JPEG decoders.
    if (w == 0 || h == 0 ||
        static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) >
            static_cast<std::uint64_t>(kMaxImagePixels)) {
        TIFFClose(tif);
        return std::nullopt;
    }

    PixelBuffer out(static_cast<int>(w), static_cast<int>(h));

    // Fast path for the straightforward 8-bit, RGB(A), contiguous, striped layout (what
    // encodeTiff writes, and most editors): read raw scanlines so straight (unassociated)
    // alpha is preserved exactly. TIFFReadRGBAImage would premultiply it — lossy.
    uint16_t spp = 0;
    uint16_t bps = 0;
    uint16_t photometric = 0;
    uint16_t planar = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    const bool simpleRgb = bps == 8 && (spp == 3 || spp == 4) && photometric == PHOTOMETRIC_RGB &&
                           planar == PLANARCONFIG_CONTIG && TIFFIsTiled(tif) == 0;

    if (simpleRgb) {
        std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * spp);
        bool ok = true;
        for (uint32_t y = 0; y < h && ok; ++y) {
            if (TIFFReadScanline(tif, row.data(), y, 0) < 0) {
                ok = false;
                break;
            }
            for (uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(x) * spp;
                const std::uint8_t a = spp == 4 ? row[i + 3] : 255;
                out.set(static_cast<int>(x), static_cast<int>(y),
                        Rgba8{row[i], row[i + 1], row[i + 2], a});
            }
        }
        // A simple-RGB TIFF is authoritative: if a scanline read fails the file is
        // corrupt — fail rather than silently taking the premultiplying RGBA fallback.
        TIFFClose(tif);
        return ok ? std::optional<PixelBuffer>(std::move(out)) : std::nullopt;
    }

    // General fallback: normalize any TIFF (palette, 1/16-bit, CMYK, tiled, ...) to a
    // packed-RGBA uint32 raster. On little-endian, TIFFGetR/G/B/A map to memory bytes
    // R,G,B,A — matching Rgba8. (Straight alpha is premultiplied here; acceptable for
    // exotic inputs we don't author.)
    auto* raster = reinterpret_cast<uint32_t*>(out.data());
    const int rc = TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 0);
    TIFFClose(tif);
    if (rc == 0) return std::nullopt;
    return out;
}

}  // namespace pe
