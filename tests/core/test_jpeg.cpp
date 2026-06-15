#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_JPEG

#include <cstddef>
#include <cstdlib>
#include <vector>

using namespace pe;

namespace {
bool near8rgb(Rgba8 a, Rgba8 b, int tol) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol;
}
}  // namespace

PE_TEST(jpeg_encode_decode_roundtrip_lossy) {
    PixelBuffer img(16, 16, Rgba8{40, 120, 200, 255});  // solid color survives JPEG well
    std::vector<std::byte> jpeg = encodeJpeg(img, 95);
    PE_CHECK(!jpeg.empty());
    // JPEG SOI marker: FF D8.
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(jpeg[0])), 0xFFu);
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(jpeg[1])), 0xD8u);

    auto decoded = decodeJpeg(jpeg);
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->width(), 16);
    PE_CHECK_EQ(decoded->height(), 16);
    PE_CHECK(near8rgb(decoded->at(8, 8), Rgba8{40, 120, 200, 255}, 4));  // lossy, within tol
}

PE_TEST(jpeg_decode_sets_alpha_opaque) {
    PixelBuffer img(8, 8, Rgba8{10, 20, 30, 0});  // input alpha 0 is dropped by JPEG
    auto decoded = decodeJpeg(encodeJpeg(img, 90));
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->at(4, 4).a, static_cast<uint8_t>(255));  // decoded alpha is opaque
}

PE_TEST(jpeg_decode_garbage_is_nullopt) {
    std::vector<std::byte> junk(128, std::byte{0x5A});
    PE_CHECK(!decodeJpeg(junk).has_value());
    PE_CHECK(!decodeJpeg(std::span<const std::byte>{}).has_value());  // empty input
}

PE_TEST(jpeg_encode_empty_is_empty) {
    PE_CHECK(encodeJpeg(PixelBuffer{}).empty());
}

PE_TEST(jpeg_quality_is_clamped) {
    PixelBuffer img(8, 8, Rgba8{200, 100, 50, 255});
    PE_CHECK(!encodeJpeg(img, 0).empty());    // clamps to 1, still a valid JPEG
    PE_CHECK(!encodeJpeg(img, 999).empty());  // clamps to 100
}

#endif  // PHOTOEDIT_HAVE_JPEG
