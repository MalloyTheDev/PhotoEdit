#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_TIFF

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace pe;

PE_TEST(tiff_encode_decode_roundtrip_lossless) {
    PixelBuffer img(5, 3);
    img.set(0, 0, Rgba8{200, 100, 50, 255});
    img.set(4, 2, Rgba8{10, 20, 30, 200});
    img.set(2, 1, Rgba8{0, 0, 0, 0});  // fully transparent

    std::vector<std::byte> tiff = encodeTiff(img);
    PE_CHECK(!tiff.empty());
    // TIFF magic: "II*\0" (little-endian) or "MM\0*" (big-endian); libtiff writes II here.
    const auto b0 = std::to_integer<unsigned char>(tiff[0]);
    const auto b1 = std::to_integer<unsigned char>(tiff[1]);
    PE_CHECK((b0 == 'I' && b1 == 'I') || (b0 == 'M' && b1 == 'M'));

    auto decoded = decodeTiff(tiff);
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->width(), 5);
    PE_CHECK_EQ(decoded->height(), 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) PE_CHECK_EQ(decoded->at(x, y), img.at(x, y));  // lossless
    }
}

PE_TEST(tiff_solid_image_roundtrip) {
    PixelBuffer img(20, 12, Rgba8{64, 128, 192, 255});
    auto decoded = decodeTiff(encodeTiff(img));
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->at(10, 6), (Rgba8{64, 128, 192, 255}));
}

PE_TEST(tiff_decode_garbage_is_nullopt) {
    std::vector<std::byte> junk(64, std::byte{0x5A});
    PE_CHECK(!decodeTiff(junk).has_value());
    PE_CHECK(!decodeTiff(std::span<const std::byte>{}).has_value());  // empty input
}

PE_TEST(tiff_encode_empty_is_empty) {
    PE_CHECK(encodeTiff(PixelBuffer{}).empty());
}

PE_TEST(tiff_decode_truncation_never_crashes) {
    // Every truncated prefix of a valid TIFF must decode to nullopt without crashing —
    // untrusted-input path (truncation lands inside the IFD / strip offsets).
    PixelBuffer img(32, 24, Rgba8{40, 80, 120, 255});
    img.set(5, 6, Rgba8{200, 10, 20, 255});
    const std::vector<std::byte> tiff = encodeTiff(img);
    PE_CHECK(!tiff.empty());
    for (std::size_t n = 0; n < tiff.size(); ++n) {
        (void)decodeTiff(std::span<const std::byte>(tiff.data(), n));
    }
    PE_CHECK(decodeTiff(tiff).has_value());  // the complete stream still decodes
}

namespace {

// Rewrite the ORIENTATION tag (0x0112) in a little-endian TIFF's first IFD. encodeTiff
// always writes ORIENTATION_TOPLEFT, so the tag is present and its value is inline (SHORT,
// one entry), which makes patching it in place enough. Returns false if the layout is not
// what we expect, so a test can never silently check nothing.
bool patchOrientation(std::vector<std::byte>& tiff, std::uint16_t value) {
    const auto u16 = [&tiff](std::size_t at) {
        return static_cast<std::uint16_t>(std::to_integer<unsigned>(tiff[at]) |
                                          (std::to_integer<unsigned>(tiff[at + 1]) << 8));
    };
    const auto u32 = [&tiff](std::size_t at) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= std::to_integer<unsigned>(tiff[at + static_cast<std::size_t>(i)])
                 << (8 * static_cast<unsigned>(i));
        }
        return v;
    };
    if (tiff.size() < 8) return false;
    if (std::to_integer<unsigned char>(tiff[0]) != 'I') return false;  // little-endian only
    const std::uint32_t ifd = u32(4);
    if (static_cast<std::size_t>(ifd) + 2 > tiff.size()) return false;
    const std::uint16_t count = u16(ifd);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t entry =
            static_cast<std::size_t>(ifd) + 2 + static_cast<std::size_t>(i) * 12;
        if (entry + 12 > tiff.size()) return false;
        if (u16(entry) != 0x0112) continue;
        tiff[entry + 8] = static_cast<std::byte>(value & 0xFF);
        tiff[entry + 9] = static_cast<std::byte>((value >> 8) & 0xFF);
        return true;
    }
    return false;
}

PixelBuffer gradientImage(int w, int h) {
    PixelBuffer img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            img.set(x, y,
                    Rgba8{static_cast<std::uint8_t>(10 + 40 * y),
                          static_cast<std::uint8_t>(5 + 20 * x), 128, 255});
        }
    }
    return img;
}

}  // namespace

PE_TEST(tiff_decode_honours_a_bottom_left_orientation) {
    // decodeTiff has two paths and only the general fallback normalized orientation. The
    // 8-bit RGB contiguous striped fast path read scanlines in storage order and wrote row
    // y to output row y, so a BOTLEFT file (several scanners, and some GIMP and ImageMagick
    // paths) decoded vertically flipped. The same image at 16 bit took the fallback and
    // decoded correctly, so the behaviour was silently inconsistent across bit depths.
    //
    // Round-trip tests cannot catch this: encodeTiff always writes TOPLEFT.
    const PixelBuffer img = gradientImage(4, 3);
    std::vector<std::byte> tiff = encodeTiff(img);
    PE_CHECK(!tiff.empty());
    PE_CHECK(patchOrientation(tiff, 4));  // ORIENTATION_BOTLEFT

    auto decoded = decodeTiff(tiff);
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->width(), 4);
    PE_CHECK_EQ(decoded->height(), 3);

    // The rows are stored bottom-up, so a correct decode reverses them: stored row 0
    // belongs at the bottom of the returned image.
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) PE_CHECK_EQ(decoded->at(x, 2 - y), img.at(x, y));
    }
}

PE_TEST(tiff_decode_honours_a_top_right_orientation) {
    const PixelBuffer img = gradientImage(4, 3);
    std::vector<std::byte> tiff = encodeTiff(img);
    PE_CHECK(patchOrientation(tiff, 2));  // ORIENTATION_TOPRIGHT: columns mirrored
    auto decoded = decodeTiff(tiff);
    PE_CHECK(decoded.has_value());
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) PE_CHECK_EQ(decoded->at(3 - x, y), img.at(x, y));
    }
}

PE_TEST(tiff_default_orientation_is_unchanged) {
    // The common case must not move: an ordinary TOPLEFT file still round-trips exactly.
    const PixelBuffer img = gradientImage(4, 3);
    auto decoded = decodeTiff(encodeTiff(img));
    PE_CHECK(decoded.has_value());
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) PE_CHECK_EQ(decoded->at(x, y), img.at(x, y));
    }
}

#endif  // PHOTOEDIT_HAVE_TIFF
