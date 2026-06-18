#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace pe;

namespace {

// Minimal big-endian PSD byte builder for tests.
struct PsdBuilder {
    std::vector<std::byte> bytes;
    void u8(std::uint8_t v) { bytes.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v >> 8));
        u8(static_cast<std::uint8_t>(v & 0xff));
    }
    void u32(std::uint32_t v) {
        u8(static_cast<std::uint8_t>(v >> 24));
        u8(static_cast<std::uint8_t>((v >> 16) & 0xff));
        u8(static_cast<std::uint8_t>((v >> 8) & 0xff));
        u8(static_cast<std::uint8_t>(v & 0xff));
    }
    void header(std::uint16_t channels, std::uint32_t w, std::uint32_t h, std::uint16_t depth,
                std::uint16_t colorMode, std::uint16_t version = 1) {
        u8('8');
        u8('B');
        u8('P');
        u8('S');
        u16(version);
        for (int i = 0; i < 6; ++i) u8(0);  // reserved
        u16(channels);
        u32(h);
        u32(w);
        u16(depth);
        u16(colorMode);
        u32(0);  // color mode data length
        u32(0);  // image resources length
        u32(0);  // layer & mask info length
    }
};

// RGB 8-bit, raw compression. `px` is row-major w*h of {r,g,b}.
std::vector<std::byte> makeRgbRaw(int w, int h,
                                  const std::vector<std::array<std::uint8_t, 3>>& px) {
    PsdBuilder b;
    b.header(3, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 8, 3);
    b.u16(0);  // raw
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < w * h; ++i)
            b.u8(px[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)]);
    }
    return b.bytes;
}

// RGB 8-bit, RLE compression. Each row is encoded as one literal run: [w-1][bytes...] (w<=128).
std::vector<std::byte> makeRgbRle(int w, int h,
                                  const std::vector<std::array<std::uint8_t, 3>>& px) {
    PsdBuilder b;
    b.header(3, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 8, 3);
    b.u16(1);  // RLE
    // Row-count table: channels*height entries; each row compresses to (1 + w) bytes.
    for (int i = 0; i < 3 * h; ++i) b.u16(static_cast<std::uint16_t>(1 + w));
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < h; ++y) {
            b.u8(static_cast<std::uint8_t>(w - 1));  // literal run of w bytes
            for (int x = 0; x < w; ++x) {
                b.u8(px[static_cast<std::size_t>(y * w + x)][static_cast<std::size_t>(c)]);
            }
        }
    }
    return b.bytes;
}

}  // namespace

PE_TEST(psd_decode_rgb_raw_roundtrip) {
    const std::vector<std::array<std::uint8_t, 3>> px = {
        {10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};
    auto img = decodePsd(makeRgbRaw(2, 2, px));
    PE_CHECK(img.has_value());
    PE_CHECK_EQ(img->width(), 2);
    PE_CHECK_EQ(img->height(), 2);
    PE_CHECK_EQ(img->at(0, 0), (Rgba8{10, 20, 30, 255}));
    PE_CHECK_EQ(img->at(1, 0), (Rgba8{40, 50, 60, 255}));
    PE_CHECK_EQ(img->at(0, 1), (Rgba8{70, 80, 90, 255}));
    PE_CHECK_EQ(img->at(1, 1), (Rgba8{100, 110, 120, 255}));
}

PE_TEST(psd_decode_rgb_rle_roundtrip) {
    const std::vector<std::array<std::uint8_t, 3>> px = {
        {1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {200, 150, 100}};
    auto img = decodePsd(makeRgbRle(2, 2, px));
    PE_CHECK(img.has_value());
    PE_CHECK_EQ(img->at(0, 0), (Rgba8{1, 2, 3, 255}));
    PE_CHECK_EQ(img->at(1, 1), (Rgba8{200, 150, 100, 255}));
}

PE_TEST(psd_decode_grayscale_with_alpha) {
    // channels=2 (gray + alpha), colorMode=1, raw.
    PsdBuilder b;
    b.header(2, 2, 1, 8, 1);
    b.u16(0);  // raw
    b.u8(50);
    b.u8(200);  // gray plane (2 px)
    b.u8(255);
    b.u8(128);  // alpha plane
    auto img = decodePsd(b.bytes);
    PE_CHECK(img.has_value());
    PE_CHECK_EQ(img->at(0, 0), (Rgba8{50, 50, 50, 255}));
    PE_CHECK_EQ(img->at(1, 0), (Rgba8{200, 200, 200, 128}));
}

PE_TEST(psd_decode_rejects_unsupported_and_malformed) {
    const std::vector<std::array<std::uint8_t, 3>> px = {{0, 0, 0}};

    PE_CHECK(!decodePsd(std::span<const std::byte>{}).has_value());  // empty

    auto bad = makeRgbRaw(1, 1, px);
    bad[0] = std::byte{'X'};  // corrupt signature
    PE_CHECK(!decodePsd(bad).has_value());

    {  // PSB (version 2) rejected
        PsdBuilder b;
        b.header(3, 1, 1, 8, 3, /*version=*/2);
        b.u16(0);
        b.u8(0);
        b.u8(0);
        b.u8(0);
        PE_CHECK(!decodePsd(b.bytes).has_value());
    }
    {  // 16-bit depth rejected
        PsdBuilder b;
        b.header(3, 1, 1, 16, 3);
        b.u16(0);
        PE_CHECK(!decodePsd(b.bytes).has_value());
    }
    {  // CMYK color mode rejected
        PsdBuilder b;
        b.header(4, 1, 1, 8, 4);
        b.u16(0);
        PE_CHECK(!decodePsd(b.bytes).has_value());
    }
    {  // oversize dimension rejected (before any allocation)
        PsdBuilder b;
        b.header(3, 40000, 40000, 8, 3);
        b.u16(0);
        PE_CHECK(!decodePsd(b.bytes).has_value());
    }
    {  // unsupported compression (ZIP) rejected
        PsdBuilder b;
        b.header(3, 1, 1, 8, 3);
        b.u16(2);  // ZIP
        PE_CHECK(!decodePsd(b.bytes).has_value());
    }
}

PE_TEST(psd_decode_truncation_never_crashes) {
    // Every truncated prefix of a valid PSD must decode to nullopt without crashing or reading
    // out of bounds — the untrusted-input contract (validated under ASan in the no-deps lane).
    const std::vector<std::array<std::uint8_t, 3>> px = {
        {10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};
    const std::vector<std::byte> full = makeRgbRaw(2, 2, px);
    for (std::size_t n = 0; n < full.size(); ++n) {
        (void)decodePsd(std::span<const std::byte>(full.data(), n));
    }
    PE_CHECK(decodePsd(full).has_value());  // the complete stream still decodes

    // Same for the RLE variant (truncation lands inside the row table / PackBits stream).
    const std::vector<std::byte> rle = makeRgbRle(2, 2, px);
    for (std::size_t n = 0; n < rle.size(); ++n) {
        (void)decodePsd(std::span<const std::byte>(rle.data(), n));
    }
    PE_CHECK(decodePsd(rle).has_value());
}

PE_TEST(psd_documentio_integration) {
    PE_CHECK(formatFromExtension("art.psd") == ImageFormat::Psd);
    PE_CHECK(formatFromExtension("/a/b/PHOTO.PSD") == ImageFormat::Psd);  // case-insensitive
    PE_CHECK(formatAvailable(ImageFormat::Psd));  // always available (dependency-free)

    const std::vector<std::array<std::uint8_t, 3>> px = {
        {11, 22, 33}, {44, 55, 66}, {77, 88, 99}, {123, 124, 125}};
    auto doc = importDocument(makeRgbRaw(2, 2, px), ImageFormat::Psd);  // flattens to one layer
    PE_CHECK(doc != nullptr);
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    PE_CHECK(pl != nullptr);
    PE_CHECK_EQ(pl->tiles().pixel(0, 0), (Rgba8{11, 22, 33, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(1, 1), (Rgba8{123, 124, 125, 255}));

    // PSD is import-only: exporting to PSD yields no bytes (no encoder).
    PE_CHECK(exportDocument(*doc, ImageFormat::Psd).empty());
}
