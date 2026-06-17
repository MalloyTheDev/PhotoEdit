#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_WEBP

#include <cstddef>
#include <vector>

using namespace pe;

PE_TEST(webp_encode_decode_roundtrip_lossless) {
    PixelBuffer img(6, 4);
    img.set(0, 0, Rgba8{200, 100, 50, 255});
    img.set(5, 3, Rgba8{10, 20, 30, 200});
    img.set(2, 1, Rgba8{0, 0, 0, 0});  // fully transparent

    std::vector<std::byte> webp = encodeWebp(img);
    PE_CHECK(!webp.empty());
    // WebP container: "RIFF" .... "WEBP".
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(webp[0])), unsigned('R'));
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(webp[1])), unsigned('I'));
    PE_CHECK_EQ(static_cast<unsigned>(std::to_integer<unsigned char>(webp[8])), unsigned('W'));

    auto decoded = decodeWebp(webp);
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->width(), 6);
    PE_CHECK_EQ(decoded->height(), 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) PE_CHECK_EQ(decoded->at(x, y), img.at(x, y));  // lossless
    }
}

PE_TEST(webp_solid_image_roundtrip) {
    PixelBuffer img(24, 16, Rgba8{64, 128, 192, 255});
    auto decoded = decodeWebp(encodeWebp(img));
    PE_CHECK(decoded.has_value());
    PE_CHECK_EQ(decoded->at(12, 8), (Rgba8{64, 128, 192, 255}));
}

PE_TEST(webp_decode_garbage_is_nullopt) {
    std::vector<std::byte> junk(64, std::byte{0x5A});
    PE_CHECK(!decodeWebp(junk).has_value());
    PE_CHECK(!decodeWebp(std::span<const std::byte>{}).has_value());  // empty input
}

PE_TEST(webp_encode_empty_is_empty) {
    PE_CHECK(encodeWebp(PixelBuffer{}).empty());
}

PE_TEST(webp_decode_truncation_never_crashes) {
    // Every truncated prefix of a valid WebP must decode to nullopt without crashing —
    // untrusted-input path (truncation can land inside the RIFF/VP8L headers).
    PixelBuffer img(32, 24, Rgba8{40, 80, 120, 255});
    img.set(5, 6, Rgba8{200, 10, 20, 255});
    const std::vector<std::byte> webp = encodeWebp(img);
    PE_CHECK(!webp.empty());
    for (std::size_t n = 0; n < webp.size(); ++n) {
        (void)decodeWebp(std::span<const std::byte>(webp.data(), n));
    }
    PE_CHECK(decodeWebp(webp).has_value());  // the complete stream still decodes
}

#endif  // PHOTOEDIT_HAVE_WEBP
