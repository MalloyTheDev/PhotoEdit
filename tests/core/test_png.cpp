#include "pe/core/Document.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_PNG

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace pe;

namespace {
// CRC-32 (ISO 3309, the variant PNG chunks use) over a byte range — lets a test re-stamp
// a patched IHDR so libpng still accepts the header.
std::uint32_t crc32Png(const std::byte* p, std::size_t n) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[i]));
        for (int k = 0; k < 8; ++k) crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}
std::uint32_t readBE32(const std::vector<std::byte>& v, std::size_t off) {
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(v[off])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(v[off + 1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(v[off + 2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<unsigned char>(v[off + 3]));
}
void writeBE32(std::vector<std::byte>& v, std::size_t off, std::uint32_t x) {
    v[off + 0] = static_cast<std::byte>((x >> 24) & 0xFFu);
    v[off + 1] = static_cast<std::byte>((x >> 16) & 0xFFu);
    v[off + 2] = static_cast<std::byte>((x >> 8) & 0xFFu);
    v[off + 3] = static_cast<std::byte>(x & 0xFFu);
}
}  // namespace

PE_TEST(png_encode_decode_roundtrip) {
    PixelBuffer img(4, 3);
    img.set(0, 0, Rgba8{255, 0, 0, 255});
    img.set(1, 0, Rgba8{0, 255, 0, 128});
    img.set(3, 2, Rgba8{10, 20, 30, 200});
    img.set(2, 1, Rgba8{0, 0, 0, 0});  // fully transparent

    std::vector<std::byte> png = encodePng(img);
    PE_CHECK(!png.empty());
    // PNG signature: 89 50 4E 47 0D 0A 1A 0A.
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(png[1])), 0x50u);  // 'P'
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(png[2])), 0x4Eu);  // 'N'

    auto decoded = decodePng(png);
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->width(), 4);
    PE_CHECK_EQ(decoded->height(), 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) PE_CHECK_EQ(decoded->at(x, y), img.at(x, y));
    }
}

PE_TEST(png_decode_garbage_is_nullopt) {
    std::vector<std::byte> junk(64, std::byte{0xAB});
    PE_CHECK(!decodePng(junk).has_value());
    PE_CHECK(!decodePng(std::span<const std::byte>{}).has_value());  // empty input
}

PE_TEST(png_encode_empty_is_empty) {
    PE_CHECK(encodePng(PixelBuffer{}).empty());
}

PE_TEST(png_solid_image_roundtrip) {
    PixelBuffer img(8, 8, Rgba8{64, 128, 192, 255});  // solid fill
    auto decoded = decodePng(encodePng(img));
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->at(5, 5), (Rgba8{64, 128, 192, 255}));
}

PE_TEST(png_document_export_import_roundtrip) {
    auto doc = Document::createBlank(Size{6, 4});
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    PE_CHECK(pl != nullptr);
    pl->tiles().fillRect(Rect{0, 0, 6, 4}, Rgba8{30, 90, 150, 255});
    pl->tiles().setPixel(2, 1, Rgba8{200, 10, 20, 255});

    std::vector<std::byte> png = exportDocumentPng(*doc);
    PE_CHECK(!png.empty());

    auto loaded = importDocumentPng(png);
    PE_CHECK(loaded != nullptr);
    PE_CHECK_EQ(loaded->canvasSize().width, 6);
    PE_CHECK_EQ(loaded->canvasSize().height, 4);
    auto* lpl = dynamic_cast<PixelLayer*>(loaded->findLayer(loaded->activeLayer()));
    PE_CHECK(lpl != nullptr);
    PE_CHECK_EQ(lpl->tiles().pixel(0, 0), (Rgba8{30, 90, 150, 255}));
    PE_CHECK_EQ(lpl->tiles().pixel(2, 1), (Rgba8{200, 10, 20, 255}));
}

PE_TEST(png_import_garbage_is_null) {
    std::vector<std::byte> junk(32, std::byte{0x7F});
    PE_CHECK(importDocumentPng(junk) == nullptr);
}

PE_TEST(png_decode_truncation_never_crashes) {
    // Every truncated prefix of a valid PNG must decode to nullopt without crashing or
    // over-reading — the decoder is on the untrusted-input path. (A larger image gives a
    // multi-chunk stream so truncation lands inside IHDR/IDAT/IEND, not just the trailer.)
    PixelBuffer img(40, 30, Rgba8{12, 34, 56, 200});
    img.set(7, 9, Rgba8{255, 0, 0, 255});
    const std::vector<std::byte> png = encodePng(img);
    PE_CHECK(!png.empty());
    for (std::size_t n = 0; n < png.size(); ++n) {
        // Just must not crash; almost all prefixes are invalid, the full blob round-trips.
        (void)decodePng(std::span<const std::byte>(png.data(), n));
    }
    PE_CHECK(decodePng(png).has_value());  // the complete stream still decodes
}

PE_TEST(png_decode_oversize_dimensions_rejected) {
    // The 64 MP decode cap (untrusted input) must reject an over-cap image at the dimension
    // check, before allocating the output buffer. Encode a tiny VALID PNG, then patch its
    // IHDR to claim 9000x9000 (81 MP > the 64 MP cap, yet each dimension is well under
    // libpng's per-dimension limit so the header still parses) and re-stamp the IHDR CRC.
    // The real IDAT/IEND remain, so begin_read succeeds and reports the patched dimensions;
    // the engine's cap is then what rejects it (finish_read is never reached).
    std::vector<std::byte> png = encodePng(PixelBuffer(4, 4, Rgba8{1, 2, 3, 255}));
    PE_CHECK(png.size() > 33);
    // PNG: 8-byte signature, then IHDR chunk — length@8, "IHDR"@12, width@16, height@20,
    // ..., CRC@29 (computed over the 17 bytes "IHDR"+data at [12,29)).
    // Self-check the CRC machinery against libpng's own IHDR CRC first: this proves the
    // offsets and crc32 are correct, so the patched header below carries a VALID CRC and
    // begin_read accepts it — otherwise begin_read would reject on a bad CRC before the cap
    // (the exact "passes for the wrong reason" trap this test must avoid).
    PE_CHECK_EQ(crc32Png(png.data() + 12, 17), readBE32(png, 29));
    writeBE32(png, 16, 9000);                           // patched width
    writeBE32(png, 20, 9000);                           // patched height
    writeBE32(png, 29, crc32Png(png.data() + 12, 17));  // valid CRC for the patched IHDR
    PE_CHECK(!decodePng(png).has_value());  // rejected by the 64 MP cap, no 324 MB alloc
}

PE_TEST(png_import_extreme_aspect_ratio_is_null_not_crash) {
    // A 1 x 400000 image is under the 64 MP decode cap but exceeds the document's
    // per-dimension canvas cap (300000); import must reject it, not dereference null.
    PixelBuffer tall(1, 400000, Rgba8{1, 2, 3, 255});
    std::vector<std::byte> png = encodePng(tall);
    PE_CHECK(!png.empty());
    PE_CHECK(importDocumentPng(png) == nullptr);
}

#endif  // PHOTOEDIT_HAVE_PNG
