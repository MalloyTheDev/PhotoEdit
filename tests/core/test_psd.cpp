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

// Build an RLE PSD from explicit per-(channel,row) PackBits byte blobs, in channel-major then row
// order (ch0row0, ch0row1, ..., ch1row0, ...). The row-count table is each blob's length, so this
// gives full control for malformed and non-canonical edge cases.
std::vector<std::byte> makeRleFromRows(std::uint16_t channels, int w, int h,
                                       std::uint16_t colorMode,
                                       const std::vector<std::vector<std::uint8_t>>& rows) {
    PsdBuilder b;
    b.header(channels, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 8, colorMode);
    b.u16(1);  // RLE
    for (const auto& row : rows) b.u16(static_cast<std::uint16_t>(row.size()));
    for (const auto& row : rows) {
        for (std::uint8_t byte : row) b.u8(byte);
    }
    return b.bytes;
}

// PackBits encoders: a literal run of the given bytes, and a repeat run of `count` copies of `v`.
std::vector<std::uint8_t> litRun(std::vector<std::uint8_t> bytes) {
    std::vector<std::uint8_t> r;
    r.push_back(static_cast<std::uint8_t>(bytes.size() - 1));  // control: count-1 literals
    for (std::uint8_t byte : bytes) r.push_back(byte);
    return r;
}
std::vector<std::uint8_t> repeatRun(int count, std::uint8_t v) {
    return {static_cast<std::uint8_t>(1 - count),
            v};  // control = 1-count (negative) => count copies
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
    {  // oversize dimension rejected (per-dimension cap, before any allocation)
        PsdBuilder b;
        b.header(3, 40000, 40000, 8, 3);
        b.u16(0);
        PE_CHECK(!decodePsd(b.bytes).has_value());
    }
    {  // both dims <= 30000 but area (100 MP) exceeds the 64 MP cap — pins the area cap
       // independently of the per-dimension cap (fires before any pixel allocation).
        PsdBuilder b;
        b.header(3, 10000, 10000, 8, 3);
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
    for (std::size_t n = 0; n < full.size(); ++n) {  // every short prefix must REJECT, not crash
        PE_CHECK(!decodePsd(std::span<const std::byte>(full.data(), n)).has_value());
    }
    PE_CHECK(decodePsd(full).has_value());  // the complete stream still decodes

    // Same for the RLE variant (truncation lands inside the row table / PackBits stream).
    const std::vector<std::byte> rle = makeRgbRle(2, 2, px);
    for (std::size_t n = 0; n < rle.size(); ++n) {
        PE_CHECK(!decodePsd(std::span<const std::byte>(rle.data(), n)).has_value());
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

PE_TEST(psd_rle_malformed_rows_rejected) {
    // (a) a literal run that over-runs the row width (w=2, run claims 4 literals) -> reject.
    {
        const std::vector<std::vector<std::uint8_t>> rows = {litRun({1, 2, 3, 4}), litRun({5, 6}),
                                                             litRun({7, 8})};  // 3 channels, h=1
        PE_CHECK(!decodePsd(makeRleFromRows(3, 2, 1, 3, rows)).has_value());
    }
    // (b) a row that decodes to fewer than w bytes (under-run: w=4, run yields 2) -> reject.
    {
        const std::vector<std::vector<std::uint8_t>> rows = {
            litRun({1, 2}), litRun({3, 4}), litRun({5, 6})};  // each fills only 2 of w=4
        PE_CHECK(!decodePsd(makeRleFromRows(3, 4, 1, 3, rows)).has_value());
    }
}

PE_TEST(psd_rle_nonsquare_mixed_runs_roundtrip) {
    // 3x2 RGB (w != h, h > 1): row 0 distinct (literal run, length 4) and row 1 uniform (replicate
    // run, length 2) => NON-UNIFORM table lengths. Pins row stride (y*w), the channel-major row
    // index (ch*h + y), plane ordering, and the negative-n replicate branch all at once.
    const std::vector<std::vector<std::uint8_t>> rows = {
        litRun({10, 20, 30}), repeatRun(3, 100),  // R: row0 distinct, row1 = three 100s
        litRun({40, 50, 60}), repeatRun(3, 110),  // G
        litRun({70, 80, 90}), repeatRun(3, 120)};
    auto img = decodePsd(makeRleFromRows(3, 3, 2, 3, rows));
    PE_CHECK(img.has_value());
    PE_CHECK_EQ(img->at(0, 0), (Rgba8{10, 40, 70, 255}));
    PE_CHECK_EQ(img->at(1, 0), (Rgba8{20, 50, 80, 255}));
    PE_CHECK_EQ(img->at(2, 0), (Rgba8{30, 60, 90, 255}));
    PE_CHECK_EQ(img->at(0, 1), (Rgba8{100, 110, 120, 255}));
    PE_CHECK_EQ(img->at(2, 1), (Rgba8{100, 110, 120, 255}));
}

PE_TEST(psd_rle_overdeclared_clen_resyncs) {
    // Grayscale 2x2. Row 0's tabled length (5) is longer than the bytes its run needs (3): the
    // decoder must skip the 2-byte tail so row 1 still decodes correctly (no cross-row desync).
    const std::vector<std::uint8_t> row0 = {1, 10, 20, 0, 0};  // litRun {10,20} (3 bytes) + 2 pad
    const std::vector<std::uint8_t> row1 = {1, 30, 40};        // litRun {30,40}
    auto img = decodePsd(makeRleFromRows(1, 2, 2, 1, {row0, row1}));
    PE_CHECK(img.has_value());
    PE_CHECK_EQ(img->at(0, 0), (Rgba8{10, 10, 10, 255}));
    PE_CHECK_EQ(img->at(1, 0), (Rgba8{20, 20, 20, 255}));
    PE_CHECK_EQ(img->at(0, 1), (Rgba8{30, 30, 30, 255}));
    PE_CHECK_EQ(img->at(1, 1), (Rgba8{40, 40, 40, 255}));
}

PE_TEST(psd_rle_noop_control_byte) {
    // A -128 control byte is a PackBits no-op; the row still decodes to its w pixels.
    const std::vector<std::uint8_t> row = {0x80, 1, 10, 20};  // no-op, then litRun {10,20}
    auto img = decodePsd(makeRleFromRows(1, 2, 1, 1, {row}));
    PE_CHECK(img.has_value());
    PE_CHECK_EQ(img->at(0, 0), (Rgba8{10, 10, 10, 255}));
    PE_CHECK_EQ(img->at(1, 0), (Rgba8{20, 20, 20, 255}));
}

PE_TEST(psd_decode_extra_spot_channel_skipped) {
    // RGB with 5 channels: R, G, B, alpha, spot. The spot channel is decoded-and-discarded; the
    // result RGBA must equal the first four planes and decoding must succeed (channel alignment
    // is preserved past the discarded channel).
    {  // raw
        PsdBuilder b;
        b.header(5, 2, 1, 8, 3);
        b.u16(0);
        b.u8(10);
        b.u8(11);  // R
        b.u8(20);
        b.u8(21);  // G
        b.u8(30);
        b.u8(31);  // B
        b.u8(40);
        b.u8(41);  // alpha
        b.u8(99);
        b.u8(98);  // spot (discarded)
        auto img = decodePsd(b.bytes);
        PE_CHECK(img.has_value());
        PE_CHECK_EQ(img->at(0, 0), (Rgba8{10, 20, 30, 40}));
        PE_CHECK_EQ(img->at(1, 0), (Rgba8{11, 21, 31, 41}));
    }
    {  // RLE: the discard path must still consume the spot channel's tabled row
        const std::vector<std::vector<std::uint8_t>> rows = {litRun({10, 11}), litRun({20, 21}),
                                                             litRun({30, 31}), litRun({40, 41}),
                                                             litRun({99, 98})};
        auto img = decodePsd(makeRleFromRows(5, 2, 1, 3, rows));
        PE_CHECK(img.has_value());
        PE_CHECK_EQ(img->at(0, 0), (Rgba8{10, 20, 30, 40}));
        PE_CHECK_EQ(img->at(1, 0), (Rgba8{11, 21, 31, 41}));
    }
}
