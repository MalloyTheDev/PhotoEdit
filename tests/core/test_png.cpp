#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_PNG

#include <cstddef>
#include <vector>

using namespace pe;

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

#endif  // PHOTOEDIT_HAVE_PNG
