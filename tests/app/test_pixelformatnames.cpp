// Exhaustive coverage of the pixel-format naming helpers.
//
// These are the derivation behind the document identity strip: it used to end in the
// literal string "RGB", so a Grayscale or 16-bit document described itself wrongly.
// A switch over an enum is exactly the kind of thing that silently stops being
// exhaustive when a mode is added, so every enumerator is named here explicitly
// rather than looped over.

#include "PixelFormatNames.hpp"
#include "pe_test.hpp"

#include <cstring>

using pe::app::bitDepthBits;
using pe::app::bitDepthName;
using pe::app::colorModeName;

PE_TEST(pixelformatnames_every_color_mode_has_a_distinct_name) {
    PE_CHECK(std::strcmp(colorModeName(pe::ColorMode::RGB), "RGB") == 0);
    PE_CHECK(std::strcmp(colorModeName(pe::ColorMode::CMYK), "CMYK") == 0);
    PE_CHECK(std::strcmp(colorModeName(pe::ColorMode::Gray), "Gray") == 0);
    PE_CHECK(std::strcmp(colorModeName(pe::ColorMode::Lab), "Lab") == 0);
    PE_CHECK(std::strcmp(colorModeName(pe::ColorMode::Indexed), "Indexed") == 0);
    PE_CHECK(std::strcmp(colorModeName(pe::ColorMode::Bitmap), "Bitmap") == 0);

    // Distinctness matters: two modes sharing a name would make the identity strip
    // lie without any test noticing.
    const pe::ColorMode modes[] = {pe::ColorMode::RGB,     pe::ColorMode::CMYK,
                                   pe::ColorMode::Gray,    pe::ColorMode::Lab,
                                   pe::ColorMode::Indexed, pe::ColorMode::Bitmap};
    for (const pe::ColorMode a : modes) {
        for (const pe::ColorMode b : modes) {
            if (a == b) continue;
            PE_CHECK(std::strcmp(colorModeName(a), colorModeName(b)) != 0);
        }
    }
}

PE_TEST(pixelformatnames_bit_depth_bits_match_the_enum) {
    // The enumerators carry their own bit counts, so the mapping must agree with them.
    PE_CHECK_EQ(bitDepthBits(pe::BitDepth::U8), 8);
    PE_CHECK_EQ(bitDepthBits(pe::BitDepth::U16), 16);
    PE_CHECK_EQ(bitDepthBits(pe::BitDepth::F32), 32);
    PE_CHECK_EQ(bitDepthBits(pe::BitDepth::U8), static_cast<int>(pe::BitDepth::U8));
    PE_CHECK_EQ(bitDepthBits(pe::BitDepth::U16), static_cast<int>(pe::BitDepth::U16));
    PE_CHECK_EQ(bitDepthBits(pe::BitDepth::F32), static_cast<int>(pe::BitDepth::F32));
}

PE_TEST(pixelformatnames_long_depth_names_are_distinct) {
    PE_CHECK(std::strcmp(bitDepthName(pe::BitDepth::U8), "8 Bits/Channel") == 0);
    PE_CHECK(std::strcmp(bitDepthName(pe::BitDepth::U16), "16 Bits/Channel") == 0);
    PE_CHECK(std::strcmp(bitDepthName(pe::BitDepth::F32), "32 Bits/Channel") == 0);
    PE_CHECK(std::strcmp(bitDepthName(pe::BitDepth::U8), bitDepthName(pe::BitDepth::U16)) != 0);
    PE_CHECK(std::strcmp(bitDepthName(pe::BitDepth::U16), bitDepthName(pe::BitDepth::F32)) != 0);
}
