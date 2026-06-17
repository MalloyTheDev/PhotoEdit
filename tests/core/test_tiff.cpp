#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_TIFF

#include <cstddef>
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

#endif  // PHOTOEDIT_HAVE_TIFF
