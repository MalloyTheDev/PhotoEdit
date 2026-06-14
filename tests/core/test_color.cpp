#include "pe/core/Color.hpp"
#include "pe_test.hpp"

#include <limits>

using namespace pe;

PE_TEST(color_roundtrip_endpoints) {
    Rgba8 black{0, 0, 0, 255};
    Rgba8 white{255, 255, 255, 255};
    PE_CHECK_EQ(toRgba8(toFloat(black)), black);
    PE_CHECK_EQ(toRgba8(toFloat(white)), white);
}

PE_TEST(color_unit_conversion) {
    PE_CHECK_NEAR(toLinearUnit(255), 1.0f);
    PE_CHECK_NEAR(toLinearUnit(0), 0.0f);
    PE_CHECK_EQ(fromUnit(1.0f), static_cast<uint8_t>(255));
    PE_CHECK_EQ(fromUnit(0.0f), static_cast<uint8_t>(0));
    // Clamping out-of-range floats.
    PE_CHECK_EQ(fromUnit(2.0f), static_cast<uint8_t>(255));
    PE_CHECK_EQ(fromUnit(-1.0f), static_cast<uint8_t>(0));
}

PE_TEST(color_premultiply_roundtrip) {
    Rgbaf c{0.8f, 0.4f, 0.2f, 0.5f};
    Rgbaf pm = premultiply(c);
    PE_CHECK_NEAR(pm.r, 0.4f);
    PE_CHECK_NEAR(pm.a, 0.5f);

    Rgbaf back = unpremultiply(pm);
    PE_CHECK_NEAR(back.r, 0.8f);
    PE_CHECK_NEAR(back.g, 0.4f);
    PE_CHECK_NEAR(back.b, 0.2f);

    // Fully transparent unpremultiplies to zero colour, not NaN.
    Rgbaf transparent = unpremultiply(Rgbaf{0.0f, 0.0f, 0.0f, 0.0f});
    PE_CHECK_NEAR(transparent.r, 0.0f);
}

PE_TEST(color16_unit_conversions) {
    PE_CHECK_NEAR(toUnit16(0), 0.0f);
    PE_CHECK_NEAR(toUnit16(65535), 1.0f);
    PE_CHECK_EQ(fromUnit16(0.0f), static_cast<uint16_t>(0));
    PE_CHECK_EQ(fromUnit16(1.0f), static_cast<uint16_t>(65535));
    // Round-to-nearest and clamp; NaN -> 0.
    PE_CHECK_EQ(fromUnit16(0.5f), static_cast<uint16_t>(32768));  // 0.5*65535+0.5 = 32768.0
    PE_CHECK_EQ(fromUnit16(2.0f), static_cast<uint16_t>(65535));  // clamp high
    PE_CHECK_EQ(fromUnit16(-1.0f), static_cast<uint16_t>(0));     // clamp low
    const float nan = std::numeric_limits<float>::quiet_NaN();
    PE_CHECK_EQ(fromUnit16(nan), static_cast<uint16_t>(0));
}

PE_TEST(color16_float_roundtrip) {
    Rgba16 c{1000, 32768, 65535, 40000};
    Rgbaf f = toFloat(c);
    PE_CHECK_NEAR(f.b, 1.0f);
    Rgba16 back = toRgba16(f);
    PE_CHECK_EQ(back.r, c.r);
    PE_CHECK_EQ(back.g, c.g);
    PE_CHECK_EQ(back.b, c.b);
    PE_CHECK_EQ(back.a, c.a);
}

PE_TEST(color16_widen_is_exact) {
    // 8->16 maps 0->0, 255->65535 with no gaps (v*257).
    Rgba16 w = to16(Rgba8{0, 128, 255, 64});
    PE_CHECK_EQ(w.r, static_cast<uint16_t>(0));
    PE_CHECK_EQ(w.g, static_cast<uint16_t>(128 * 257));  // 32896
    PE_CHECK_EQ(w.b, static_cast<uint16_t>(65535));
    PE_CHECK_EQ(w.a, static_cast<uint16_t>(64 * 257));
}

PE_TEST(color16_8_16_8_roundtrips_losslessly) {
    for (int v = 0; v <= 255; ++v) {
        const auto u = static_cast<uint8_t>(v);
        Rgba8 round = to8(to16(Rgba8{u, u, u, u}));
        PE_CHECK_EQ(round.r, u);
        PE_CHECK_EQ(round.a, u);
    }
    // 16->8 narrowing never exceeds 255 even at full scale.
    PE_CHECK_EQ(to8(Rgba16{65535, 65535, 65535, 65535}).r, static_cast<uint8_t>(255));
}
